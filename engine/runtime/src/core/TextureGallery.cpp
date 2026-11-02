#include "TextureGallery.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"
#include "ptv/render/GltfSkinnedLoad.h"

namespace ptv {

bool skinned_bounds(const std::vector<gltf_mesh_load::SkinnedVertex>& verts,
                    const std::vector<Mat4>& palette, Vec3& centre, float& radius) {
    Vec3 lo(1e30f), hi(-1e30f);
    bool any = false;
    for (const auto& v : verts) {
        Mat4 m(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (v.weights[k] == 0 || v.joints[k] >= palette.size()) continue;
            const float w = static_cast<float>(v.weights[k]) / 255.0f;
            m += palette[v.joints[k]] * w;
            wsum += w;
        }
        if (wsum <= 0.0f) m = Mat4(1.0f);
        const Vec3 p(m * Vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f));
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        any = true;
    }
    if (!any) return false;
    centre = 0.5f * (lo + hi);
    radius = std::max(1e-3f, 0.5f * glm::length(hi - lo));
    return true;
}

void TextureGallery::start(World& world, Renderer& renderer) {
    started_ = true;
    entries_.clear();
    std::set<std::string> seen;
    auto view = world.registry().view<const CMesh>();
    for (auto e : view) {
        const auto& cm = view.get<const CMesh>(e);
        const std::string path = cm.lod_paths.empty() ? cm.glb_path : cm.lod_paths.front();
        if (path.empty() || !seen.insert(path).second) continue;
        Entry entry;
        entry.path = path;
        const bool rigged = world.registry().all_of<CSkeleton>(e) || cm.skinned;
        if (rigged) {
            gltf_mesh_load::ModelFrame frame{};
            if (cm.has_model_frame) {
                frame.valid = true;
                frame.yaw_offset_rad = cm.model_yaw_offset_rad;
                frame.sole_y = cm.model_sole_y;
                frame.height = cm.model_height;
                frame.unit_scale = cm.unit_scale;
            }
            gltf_mesh_load::SkeletonData skel;
            entry.slot = renderer.load_glb_skinned(path, cm.target_scale_meters, skel,
                                                   frame.valid ? &frame : nullptr);
            if (entry.slot >= 0 && skel.joint_count() > 0) {
                // Bind pose: what the asset looks like before any clip plays.
                gltf_mesh_load::build_joint_palette(skel, skel.bind_pose, entry.palette);
                entry.skinned = true;
            } else {
                entry.slot = -1;
            }
        } else {
            entry.slot = renderer.load_glb(path, cm.target_scale_meters);
        }
        if (entry.slot < 0) {
            PTV_WARN("texgallery: could not load {}", path);
            continue;
        }
        entry.info = renderer.mesh_slot_info(entry.slot);
        if (entry.skinned) {
            // Frame the bind pose as the palette draws it, measured from the
            // CPU-side vertices (verification only, once per asset).
            std::vector<gltf_mesh_load::SkinnedVertex> verts;
            std::vector<std::uint32_t> indices;
            gltf_mesh_load::SkeletonData cpu_skel;
            std::string err;
            gltf_mesh_load::ModelFrame frame{};
            if (cm.has_model_frame) {
                frame.valid = true;
                frame.yaw_offset_rad = cm.model_yaw_offset_rad;
                frame.sole_y = cm.model_sole_y;
                frame.height = cm.model_height;
                frame.unit_scale = cm.unit_scale;
            }
            Vec3 c(0.0f);
            float r = entry.info.radius;
            if (gltf_mesh_load::load_glb_skinned(path, cm.target_scale_meters, verts, indices,
                                                 cpu_skel, err, frame.valid ? &frame : nullptr) &&
                skinned_bounds(verts, entry.palette, c, r)) {
                entry.info.center = c;
                entry.info.radius = r;
            } else {
                PTV_WARN("texgallery: could not measure {} ({}); framing from raw bounds", path,
                         err);
            }
        }
        entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.path < b.path; });
    PTV_INFO("texgallery: begin {} (settle {:.0f} s, {:.0f} s per asset)", entries_.size(),
             kSettleS, kSlotS);
}

void TextureGallery::update(float dt) {
    if (!started_ || done_) return;
    t_ += dt;
    const int idx = t_ < kSettleS ? -1 : static_cast<int>((t_ - kSettleS) / kSlotS);
    if (idx == shown_) return;
    shown_ = idx;
    if (idx >= static_cast<int>(entries_.size())) {
        done_ = true;
        PTV_INFO("texgallery: done");
        return;
    }
    if (idx >= 0) {
        const Entry& e = entries_[static_cast<size_t>(idx)];
        PTV_INFO("texgallery: show {}/{} {} textured={} missing={} tris={} skinned={}", idx + 1,
                 entries_.size(), e.path, e.info.textured, e.info.missing, e.info.triangles,
                 e.skinned ? 1 : 0);
    }
}

void TextureGallery::build_items(std::vector<Renderer::DrawItem>& items, Renderer& renderer,
                                 const Vec3& head_pos, const Vec3& head_forward_xz) const {
    items.clear();
    if (started_ && !done_) renderer.drop_overlays();
    if (!started_ || done_ || shown_ < 0 ||
        shown_ >= static_cast<int>(entries_.size())) {
        return;
    }
    const Entry& e = entries_[static_cast<size_t>(shown_)];
    Vec3 fwd = head_forward_xz;
    fwd.y = 0.0f;
    if (glm::length(fwd) < 1e-4f) fwd = Vec3(0.0f, 0.0f, -1.0f);
    fwd = glm::normalize(fwd);
    const float radius = std::max(1e-3f, e.info.radius);
    const float scale = 0.32f / radius;
    const Vec3 pos = head_pos + fwd * 1.1f;
    // Face the viewer, then turn a three-quarter angle so more than one side
    // shows; a slight downward tilt reveals the top.
    const float yaw_to_viewer = std::atan2(fwd.x, fwd.z);
    Mat4 m = glm::translate(Mat4(1.0f), pos) *
             glm::rotate(Mat4(1.0f), yaw_to_viewer + glm::radians(35.0f), Vec3(0, 1, 0)) *
             glm::rotate(Mat4(1.0f), glm::radians(15.0f), Vec3(1, 0, 0)) *
             glm::scale(Mat4(1.0f), Vec3(scale)) *
             glm::translate(Mat4(1.0f), -e.info.center);
    Renderer::DrawItem it{};
    it.model = m;
    it.mesh_slot = e.slot;
    it.color = Vec4(1.0f);
    if (e.skinned) {
        it.joint_base = renderer.push_joint_palette(e.palette.data(),
                                                    static_cast<int>(e.palette.size()));
        it.joint_count = static_cast<int>(e.palette.size());
    }
    items.push_back(it);
}

}  // namespace ptv
