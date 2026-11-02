// Skinned GLB loader: skins, node hierarchy, JOINTS_0 / WEIGHTS_0,
// inverse bind matrices and animation clips.
//
// Split from GltfMeshLoad.cpp so the static path -- which every Tripo asset
// and the whole world-gen pipeline still uses -- keeps its 40-byte vertex and
// stays untouched. Shared accessor plumbing lives in GltfAccessors.h.
//
// Coordinate-space contract, which the rest of the skinning tier depends on:
//
//   * Vertices are left in the skin's own space. The glTF spec is explicit
//     that a skinned mesh node's transform MUST be ignored, because the joint
//     matrices already place the mesh. Applying it would double-transform.
//   * ``palette[j] = jointWorld[j] * inverseBind[j]``. Our model matrix comes
//     from the ECS ``CTransform``, not from the glTF node, so the usual
//     ``inverse(globalTransformOfMeshNode)`` term -- which exists only to
//     cancel a node transform the renderer would otherwise re-apply -- is
//     not wanted here. A consequence worth knowing: in bind pose every
//     palette entry is the identity, so a rigged mesh drawn with an identity
//     palette is pixel-identical to the same mesh loaded as static geometry.
//   * Positions are uniformly scaled to ``target_scale_meters`` after
//     loading. Since the joint matrices are built in unscaled space, the pose
//     system conjugates them by that scale (see ``unit_scale``), which for a
//     uniform scale reduces to scaling each matrix's translation column.

#include "ptv/render/GltfSkinnedLoad.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "GltfAccessors.h"
#include "GltfMaterials.h"
#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"

namespace ptv::gltf_mesh_load {
namespace {

using namespace ptv::gltf_accessors;

constexpr std::uint32_t kGlbMagic = 0x46546C67u;  // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534Au;  // "JSON"

bool parse_glb(const std::string& path, tinygltf::Model& model,
               std::string& err, bool want_image_bytes = false) {
    auto bytes = asset_source().map(path);
    if (!bytes) {
        err = "asset source could not read GLB: " + path;
        return false;
    }
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(want_image_bytes ? capture_encoded_image_loader
                                           : noop_image_loader,
                          nullptr);
    std::string warn;
    const std::string base_dir =
        std::filesystem::path(path).parent_path().string();
    if (!loader.LoadBinaryFromMemory(&model, &err, &warn, bytes->data,
                                     static_cast<unsigned int>(bytes->size),
                                     base_dir)) {
        if (!warn.empty()) PTV_WARN("tinygltf warn: {}", warn);
        return false;
    }
    if (!warn.empty()) PTV_WARN("tinygltf warn: {}", warn);
    return true;
}

/** Split a 4x4 affine into translation / rotation / non-uniform scale.
 *
 * Hand-rolled rather than glm::decompose because we do not want the skew and
 * perspective terms, and because a mirrored joint (negative determinant) has
 * to be folded into the scale rather than silently producing a rotation that
 * flips winding. */
JointPose decompose_trs(const Mat4& m) {
    JointPose out;
    out.translation = Vec3(m[3]);

    Vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    Vec3 scale(glm::length(c0), glm::length(c1), glm::length(c2));
    if (glm::determinant(Mat3(m)) < 0.0f) {
        scale.x = -scale.x;
        c0 = -c0;
    }
    if (scale.x > 1e-8f) c0 /= std::abs(scale.x);
    if (scale.y > 1e-8f) c1 /= scale.y;
    if (scale.z > 1e-8f) c2 /= scale.z;
    out.scale = scale;

    Mat3 rot(c0, c1, c2);
    out.rotation = glm::normalize(glm::quat_cast(rot));
    return out;
}

/** Pick the skin to load, plus every (node, mesh) pair bound to it. */
struct SkinSelection {
    int skin = -1;
    std::vector<int> mesh_nodes;
    int skipped_other_skins = 0;
};

SkinSelection select_skin(const tinygltf::Model& model) {
    SkinSelection sel;
    std::unordered_set<int> other;
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        if (n.skin < 0 || n.mesh < 0) continue;
        if (sel.skin < 0) sel.skin = n.skin;
        if (n.skin == sel.skin) {
            sel.mesh_nodes.push_back(static_cast<int>(i));
        } else {
            other.insert(n.skin);
        }
    }
    sel.skipped_other_skins = static_cast<int>(other.size());
    return sel;
}

/** Build the joint hierarchy, bind pose and inverse bind matrices.
 *
 * Joint indices are preserved exactly as the glTF skin lists them, because
 * ``JOINTS_0`` refers to that ordering; ``order`` carries the parent-first
 * traversal instead of renumbering. */
bool build_skeleton(const tinygltf::Model& model, int skin_index,
                    SkeletonData& out, std::string& err) {
    const auto& skin = model.skins[static_cast<size_t>(skin_index)];
    const size_t n = skin.joints.size();
    if (n == 0) {
        err = "skin has no joints";
        return false;
    }
    if (n > static_cast<size_t>(kMaxJointsPerSkin)) {
        err = "skin has " + std::to_string(n) + " joints, exceeding the " +
              std::to_string(kMaxJointsPerSkin) +
              "-joint limit imposed by the byte-packed JOINTS_0 vertex "
              "attribute";
        return false;
    }

    std::unordered_map<int, int> node_to_joint;
    for (size_t j = 0; j < n; ++j) {
        const int node = skin.joints[j];
        if (node < 0 || static_cast<size_t>(node) >= model.nodes.size()) {
            err = "skin joint " + std::to_string(j) +
                  " references a missing node";
            return false;
        }
        node_to_joint[node] = static_cast<int>(j);
    }

    const std::vector<int> node_parent = build_node_parents(model);

    out.parents.assign(n, -1);
    out.bind_pose.assign(n, JointPose{});
    out.names.assign(n, std::string{});
    out.name_to_joint.clear();

    for (size_t j = 0; j < n; ++j) {
        const int node = skin.joints[j];
        // Fold any non-joint ancestors (a Blender "Armature" node, say) into
        // this joint's local bind transform so their placement is not lost.
        Mat4 local = node_local_matrix(model.nodes[static_cast<size_t>(node)]);
        int p = node_parent[static_cast<size_t>(node)];
        size_t guard = 0;
        while (p >= 0 && node_to_joint.find(p) == node_to_joint.end() &&
               guard++ <= model.nodes.size()) {
            local = node_local_matrix(model.nodes[static_cast<size_t>(p)]) * local;
            p = node_parent[static_cast<size_t>(p)];
        }
        out.parents[j] = (p >= 0) ? node_to_joint[p] : -1;
        out.bind_pose[j] = decompose_trs(local);

        std::string name = model.nodes[static_cast<size_t>(node)].name;
        if (name.empty()) name = "joint_" + std::to_string(j);
        out.names[j] = name;
        // First writer wins so duplicate rig names resolve deterministically.
        out.name_to_joint.emplace(name, static_cast<int>(j));
    }

    // Parent-first traversal order over the original joint indices.
    std::vector<std::vector<int>> children(n);
    std::vector<int> roots;
    for (size_t j = 0; j < n; ++j) {
        if (out.parents[j] < 0) {
            roots.push_back(static_cast<int>(j));
        } else {
            children[static_cast<size_t>(out.parents[j])].push_back(
                static_cast<int>(j));
        }
    }
    out.order.clear();
    out.order.reserve(n);
    std::vector<int> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const int j = stack.back();
        stack.pop_back();
        out.order.push_back(j);
        const auto& kids = children[static_cast<size_t>(j)];
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    if (out.order.size() != n) {
        err = "joint hierarchy is cyclic or disconnected (" +
              std::to_string(out.order.size()) + " of " + std::to_string(n) +
              " joints reachable)";
        return false;
    }

    // Inverse bind matrices. The accessor is optional; when absent the spec
    // says to treat every matrix as identity.
    out.inverse_bind.assign(n, Mat4(1.0f));
    if (skin.inverseBindMatrices >= 0) {
        std::vector<Mat4> ibm;
        if (!read_accessor_mat4(model, skin.inverseBindMatrices, ibm, &err)) {
            return false;
        }
        if (ibm.size() < n) {
            err = "inverseBindMatrices has " + std::to_string(ibm.size()) +
                  " entries for " + std::to_string(n) + " joints";
            return false;
        }
        std::copy_n(ibm.begin(), n, out.inverse_bind.begin());
    }
    return true;
}

AnimInterp parse_interp(const std::string& s) {
    if (s == "STEP") return AnimInterp::Step;
    if (s == "CUBICSPLINE") return AnimInterp::CubicSpline;
    return AnimInterp::Linear;
}

/** The non-joint ancestors folded into a root joint's bind pose, as one TRS.
 *
 * ``build_skeleton`` folds them (a Blender/Uthana "Armature" node, typically
 * scale 0.01 and a 90 degree X rotation) into the root joint's bind
 * transform. An animation channel REPLACES that joint's local transform, so
 * its keys must carry the same prefix or an animated root would be drawn in
 * the ancestor's units and axes -- 100x too large and rotated. */
struct RootPrefix {
    Vec3 translation{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float scale{1.0f};
};

std::unordered_map<int, RootPrefix> root_prefixes(
    const tinygltf::Model& model,
    const std::unordered_map<int, int>& node_to_joint) {
    std::unordered_map<int, RootPrefix> out;
    const std::vector<int> node_parent = build_node_parents(model);
    for (const auto& [node, joint] : node_to_joint) {
        int p = node_parent[static_cast<size_t>(node)];
        if (p < 0 || node_to_joint.count(p) != 0) continue;  // not a root, or no ancestor
        Mat4 prefix(1.0f);
        size_t guard = 0;
        while (p >= 0 && node_to_joint.count(p) == 0 &&
               guard++ <= model.nodes.size()) {
            prefix = node_local_matrix(model.nodes[static_cast<size_t>(p)]) * prefix;
            p = node_parent[static_cast<size_t>(p)];
        }
        const JointPose trs = decompose_trs(prefix);
        RootPrefix rp;
        rp.translation = trs.translation;
        rp.rotation = trs.rotation;
        const Vec3 s = glm::abs(trs.scale);
        rp.scale = (s.x + s.y + s.z) / 3.0f;
        if (std::abs(s.x - s.y) > 1e-4f * rp.scale ||
            std::abs(s.x - s.z) > 1e-4f * rp.scale) {
            PTV_WARN(
                "skeleton root joint {} sits under a non-uniformly scaled node "
                "({:.4f}, {:.4f}, {:.4f}); its animation uses the average scale",
                joint, s.x, s.y, s.z);
        }
        const bool identity =
            glm::length(rp.translation) < 1e-7f && std::abs(rp.scale - 1.0f) < 1e-6f &&
            std::abs(std::abs(rp.rotation.w) - 1.0f) < 1e-6f;
        if (!identity) out.emplace(joint, rp);
    }
    return out;
}

/** Apply a root prefix to one channel's keys. CUBICSPLINE tangents get the
 *  same linear part but not the constant translation. */
void bake_prefix(const RootPrefix& rp, AnimChannel& ch) {
    const size_t comps = (ch.path == AnimPath::Rotation) ? 4u : 3u;
    const bool cubic = ch.interp == AnimInterp::CubicSpline;
    const size_t count = ch.values.size() / comps;
    for (size_t k = 0; k < count; ++k) {
        float* v = &ch.values[k * comps];
        const bool is_value = !cubic || (k % 3u) == 1u;
        if (ch.path == AnimPath::Translation) {
            const Vec3 t(v[0], v[1], v[2]);
            const Vec3 r = rp.rotation * (rp.scale * t) +
                           (is_value ? rp.translation : Vec3(0.0f));
            v[0] = r.x;
            v[1] = r.y;
            v[2] = r.z;
        } else if (ch.path == AnimPath::Rotation) {
            // glTF stores x, y, z, w; glm::quat's constructor takes w first.
            // Left-multiplying by a constant rotation is linear, so values and
            // spline tangents transform the same way.
            const Quat q(v[3], v[0], v[1], v[2]);
            const Quat r = rp.rotation * q;
            v[0] = r.x;
            v[1] = r.y;
            v[2] = r.z;
            v[3] = r.w;
        } else {
            v[0] *= rp.scale;
            v[1] *= rp.scale;
            v[2] *= rp.scale;
        }
    }
}

/** Fold a model-frame pivot into every root joint: its bind pose and the keys
 *  of any clip that animates it. Applied on top of the ancestor prefix that
 *  ``build_skeleton`` / ``parse_animations`` already folded, so the result is
 *  ``pivot * ancestors * root``. */
void apply_frame_pivot(const ModelFrame& frame, SkeletonData& out) {
    const Mat4 pivot = frame.pivot();
    RootPrefix rp;
    rp.translation = Vec3(pivot[3]);
    rp.rotation = glm::angleAxis(frame.yaw_offset_rad, Vec3(0.0f, 1.0f, 0.0f));
    rp.scale = 1.0f;
    for (size_t j = 0; j < out.joint_count(); ++j) {
        if (out.parents[j] >= 0) continue;
        out.bind_pose[j] = decompose_trs(pivot * out.bind_pose[j].matrix());
        for (auto& clip : out.clips) {
            for (auto& ch : clip.channels) {
                if (ch.joint == static_cast<int>(j)) bake_prefix(rp, ch);
            }
        }
    }
}

void parse_animations(const tinygltf::Model& model,
                      const std::unordered_map<int, int>& node_to_joint,
                      SkeletonData& out) {
    const auto prefixes = root_prefixes(model, node_to_joint);
    for (size_t a = 0; a < model.animations.size(); ++a) {
        const auto& anim = model.animations[a];
        AnimationClip clip;
        clip.name = anim.name.empty() ? ("clip_" + std::to_string(a))
                                      : anim.name;

        for (const auto& ch : anim.channels) {
            auto jit = node_to_joint.find(ch.target_node);
            if (jit == node_to_joint.end()) continue;  // not part of this skin
            if (ch.sampler < 0 ||
                static_cast<size_t>(ch.sampler) >= anim.samplers.size()) {
                continue;
            }
            const auto& smp = anim.samplers[static_cast<size_t>(ch.sampler)];

            AnimChannel out_ch;
            out_ch.joint = jit->second;
            if (ch.target_path == "translation") {
                out_ch.path = AnimPath::Translation;
            } else if (ch.target_path == "scale") {
                out_ch.path = AnimPath::Scale;
            } else if (ch.target_path == "rotation") {
                out_ch.path = AnimPath::Rotation;
            } else {
                continue;  // weights / morph targets are not supported
            }
            out_ch.interp = parse_interp(smp.interpolation);

            std::string err;
            size_t comps = 0;
            if (!read_accessor_floats(model, smp.input, out_ch.times, &comps,
                                      &err) ||
                comps != 1) {
                PTV_WARN("animation '{}': unreadable time accessor ({})",
                         clip.name, err);
                continue;
            }
            if (!read_accessor_floats(model, smp.output, out_ch.values, &comps,
                                      &err)) {
                PTV_WARN("animation '{}': unreadable output accessor ({})",
                         clip.name, err);
                continue;
            }
            const size_t expected =
                (out_ch.path == AnimPath::Rotation) ? 4u : 3u;
            if (comps != expected) continue;

            const size_t per_key =
                (out_ch.interp == AnimInterp::CubicSpline) ? 3u : 1u;
            if (out_ch.times.empty() ||
                out_ch.values.size() < out_ch.times.size() * per_key * comps) {
                continue;
            }
            const auto pre = prefixes.find(out_ch.joint);
            if (pre != prefixes.end()) bake_prefix(pre->second, out_ch);
            clip.duration = std::max(clip.duration, out_ch.times.back());
            clip.channels.push_back(std::move(out_ch));
        }

        if (!clip.channels.empty()) out.clips.push_back(std::move(clip));
    }
}

/** Longest AABB axis across the skin's POSITION accessors, taken from the
 *  accessors' required min/max so no vertex data has to be decoded. */
float longest_axis_from_accessors(const tinygltf::Model& model,
                                  const std::vector<int>& mesh_nodes) {
    Vec3 lo(1e30f), hi(-1e30f);
    bool any = false;
    for (int node : mesh_nodes) {
        const int mesh = model.nodes[static_cast<size_t>(node)].mesh;
        if (mesh < 0 || static_cast<size_t>(mesh) >= model.meshes.size())
            continue;
        for (const auto& prim : model.meshes[static_cast<size_t>(mesh)].primitives) {
            if (!(prim.mode == TINYGLTF_MODE_TRIANGLES || prim.mode == -1))
                continue;
            auto it = prim.attributes.find("POSITION");
            if (it == prim.attributes.end()) continue;
            Vec3 pmin, pmax;
            if (!accessor_position_bounds(model, it->second, &pmin, &pmax))
                continue;
            lo = glm::min(lo, pmin);
            hi = glm::max(hi, pmax);
            any = true;
        }
    }
    if (!any) return 0.0f;
    const Vec3 ext = hi - lo;
    return std::max(ext.x, std::max(ext.y, ext.z));
}

void compute_vertex_normals(const std::vector<std::uint32_t>& indices,
                            std::vector<SkinnedVertex>& verts) {
    for (auto& v : verts) v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t ia = indices[i], ib = indices[i + 1],
                            ic = indices[i + 2];
        if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size())
            continue;
        const float* a = verts[ia].pos;
        const float* b = verts[ib].pos;
        const float* c = verts[ic].pos;
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float nx = e1[1] * e2[2] - e1[2] * e2[1];
        const float ny = e1[2] * e2[0] - e1[0] * e2[2];
        const float nz = e1[0] * e2[1] - e1[1] * e2[0];
        for (std::uint32_t id : {ia, ib, ic}) {
            verts[id].normal[0] += nx;
            verts[id].normal[1] += ny;
            verts[id].normal[2] += nz;
        }
    }
    for (auto& v : verts) {
        const float len = std::sqrt(v.normal[0] * v.normal[0] +
                                    v.normal[1] * v.normal[1] +
                                    v.normal[2] * v.normal[2]);
        if (len > 1e-8f) {
            v.normal[0] /= len;
            v.normal[1] /= len;
            v.normal[2] /= len;
        } else {
            v.normal[0] = 0.0f;
            v.normal[1] = 1.0f;
            v.normal[2] = 0.0f;
        }
    }
}

/** Quantize four float influences to bytes summing to exactly 255.
 *
 * Exactness matters: the shader treats R8G8B8A8_UNORM weights as a partition
 * of unity, and a sum of 254 or 256 shrinks or inflates the vertex by a
 * fraction of a percent, which reads as surface shimmer on a deforming mesh.
 * Any rounding remainder is pushed onto the dominant influence, where it is
 * least visible. */
void quantize_weights(const float in[4], std::uint8_t out[4]) {
    float sum = in[0] + in[1] + in[2] + in[3];
    float w[4];
    if (sum > 1e-8f) {
        for (int k = 0; k < 4; ++k) w[k] = in[k] / sum;
    } else {
        // Degenerate binding: pin the vertex entirely to its first joint
        // rather than letting it collapse to the origin.
        w[0] = 1.0f;
        w[1] = w[2] = w[3] = 0.0f;
    }
    int q[4];
    int total = 0;
    int dominant = 0;
    for (int k = 0; k < 4; ++k) {
        q[k] = static_cast<int>(std::lround(w[k] * 255.0f));
        q[k] = std::clamp(q[k], 0, 255);
        total += q[k];
        if (w[k] > w[dominant]) dominant = k;
    }
    q[dominant] = std::clamp(q[dominant] + (255 - total), 0, 255);
    for (int k = 0; k < 4; ++k) out[k] = static_cast<std::uint8_t>(q[k]);
}

std::array<float, 4> material_base_color(const tinygltf::Model& model,
                                         int material_idx) {
    std::array<float, 4> c{1.0f, 1.0f, 1.0f, 1.0f};
    if (material_idx < 0 ||
        static_cast<size_t>(material_idx) >= model.materials.size()) {
        return c;
    }
    const auto& bcf =
        model.materials[static_cast<size_t>(material_idx)]
            .pbrMetallicRoughness.baseColorFactor;
    if (bcf.size() >= 3) {
        c[0] = static_cast<float>(bcf[0]);
        c[1] = static_cast<float>(bcf[1]);
        c[2] = static_cast<float>(bcf[2]);
        c[3] = (bcf.size() >= 4) ? static_cast<float>(bcf[3]) : 1.0f;
    }
    return c;
}

}  // namespace

Mat4 ModelFrame::pivot() const {
    const Mat4 rot =
        glm::rotate(Mat4(1.0f), yaw_offset_rad, Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 drop = glm::translate(
        Mat4(1.0f), Vec3(0.0f, -(sole_y + 0.5f * height), 0.0f));
    return drop * rot;
}

float ModelFrame::scale_for(float target_scale_meters) const {
    if (unit_scale > 1e-8f) return unit_scale;
    if (height > 1e-6f && target_scale_meters > 1e-4f) {
        return target_scale_meters / height;
    }
    return 1.0f;
}

void build_joint_palette(const SkeletonData& skeleton,
                         const std::vector<JointPose>& local_pose,
                         std::vector<Mat4>& out_palette) {
    const size_t n = skeleton.joint_count();
    out_palette.assign(n, Mat4(1.0f));
    if (n == 0 || local_pose.size() < n) return;

    std::vector<Mat4> world(n, Mat4(1.0f));
    for (int j : skeleton.order) {
        const size_t ji = static_cast<size_t>(j);
        const Mat4 local = local_pose[ji].matrix();
        const int parent = skeleton.parents[ji];
        world[ji] = (parent >= 0) ? world[static_cast<size_t>(parent)] * local
                                  : local;
        Mat4 m = world[ji] * skeleton.inverse_bind[ji];
        if (skeleton.unit_scale != 1.0f) {
            m[3] = Vec4(Vec3(m[3]) * skeleton.unit_scale, 1.0f);
        }
        out_palette[ji] = m;
    }
}

bool glb_probe_skin(const std::string& path, bool* out_has_skin,
                    int* out_joint_count, std::string& err) {
    if (out_has_skin) *out_has_skin = false;
    if (out_joint_count) *out_joint_count = 0;

    auto view = asset_source().map(path);
    if (!view) {
        err = "asset source could not read GLB: " + path;
        return false;
    }
    const std::uint8_t* data = view->data;
    const std::size_t size = view->size;
    if (size < 20) {
        err = "GLB shorter than its own header";
        return false;
    }
    std::uint32_t magic = 0, chunk_len = 0, chunk_type = 0;
    std::memcpy(&magic, data, 4);
    if (magic != kGlbMagic) {
        err = "not a binary glTF (bad magic)";
        return false;
    }
    std::memcpy(&chunk_len, data + 12, 4);
    std::memcpy(&chunk_type, data + 16, 4);
    if (chunk_type != kChunkJson || 20u + chunk_len > size) {
        err = "first GLB chunk is not a well-formed JSON chunk";
        return false;
    }

    // Deliberately does NOT go through tinygltf: that would copy the whole BIN
    // chunk (tens of MB for a Tripo asset) just to answer a yes/no question
    // the scene loader asks once per mesh.
    try {
        const auto doc = nlohmann::json::parse(data + 20, data + 20 + chunk_len);
        const auto skins = doc.find("skins");
        if (skins == doc.end() || !skins->is_array() || skins->empty()) {
            return true;  // valid, simply not skinned
        }
        if (out_has_skin) *out_has_skin = true;
        if (out_joint_count) {
            const auto joints = (*skins)[0].find("joints");
            if (joints != (*skins)[0].end() && joints->is_array()) {
                *out_joint_count = static_cast<int>(joints->size());
            }
        }
        return true;
    } catch (const nlohmann::json::exception& e) {
        err = std::string("GLB JSON chunk did not parse: ") + e.what();
        return false;
    }
}

/** How much bigger the bind pose is DRAWN than its raw vertices: the scale of
 *  ``joint_world * inverse_bind`` at bind. It is 1 when the inverse binds
 *  cancel every ancestor of the skeleton (Tripo, Uthana), and 100 for the
 *  Sketchfab default hand, whose inverse binds leave out its 100x "Armature"
 *  node -- sizing that hand from its raw vertices drew it 20 m long, a room
 *  the player stood inside and could not see. */
float bind_palette_gain(const SkeletonData& skeleton) {
    SkeletonData raw = skeleton;
    raw.unit_scale = 1.0f;
    std::vector<Mat4> pal;
    build_joint_palette(raw, raw.bind_pose, pal);
    std::vector<float> gains;
    for (const auto& m : pal) {
        const float g = glm::length(Vec3(m[0]));
        if (std::isfinite(g) && g > 1e-6f) gains.push_back(g);
    }
    if (gains.empty()) return 1.0f;
    std::nth_element(gains.begin(), gains.begin() + gains.size() / 2, gains.end());
    return gains[gains.size() / 2];
}

bool load_glb_skeleton(const std::string& path, float target_scale_meters,
                       SkeletonData& out_skeleton, std::string& err,
                       const ModelFrame* frame) {
    out_skeleton = SkeletonData{};
    tinygltf::Model model;
    if (!parse_glb(path, model, err)) return false;

    const SkinSelection sel = select_skin(model);
    if (sel.skin < 0 || model.skins.empty()) {
        err = "GLB contains no skinned mesh node";
        return false;
    }
    if (!build_skeleton(model, sel.skin, out_skeleton, err)) return false;

    std::unordered_map<int, int> node_to_joint;
    const auto& skin = model.skins[static_cast<size_t>(sel.skin)];
    for (size_t j = 0; j < skin.joints.size(); ++j) {
        node_to_joint[skin.joints[j]] = static_cast<int>(j);
    }
    parse_animations(model, node_to_joint, out_skeleton);

    if (frame != nullptr && frame->valid) {
        apply_frame_pivot(*frame, out_skeleton);
        out_skeleton.unit_scale = frame->scale_for(target_scale_meters);
        return true;
    }
    const float extent =
        longest_axis_from_accessors(model, sel.mesh_nodes) * bind_palette_gain(out_skeleton);
    out_skeleton.unit_scale =
        (target_scale_meters > 1e-4f && extent > 1e-6f)
            ? (target_scale_meters / extent)
            : 1.0f;
    return true;
}

bool load_glb_skinned(const std::string& path, float target_scale_meters,
                      std::vector<SkinnedVertex>& out_verts,
                      std::vector<std::uint32_t>& out_indices,
                      SkeletonData& out_skeleton, std::string& err,
                      const ModelFrame* frame, MeshMaterials* out_materials,
                      const LoadOptions& options) {
    out_verts.clear();
    out_indices.clear();
    out_skeleton = SkeletonData{};

    tinygltf::Model model;
    if (!parse_glb(path, model, err, options.want_image_bytes)) return false;

    MeshMaterials local_mats;
    MeshMaterials& mats = out_materials ? *out_materials : local_mats;
    mats = MeshMaterials{};
    std::vector<gltf_materials::UvTransform> uv_xforms;
    gltf_materials::parse_materials(model, options.want_image_bytes, mats, uv_xforms);

    const SkinSelection sel = select_skin(model);
    if (sel.skin < 0 || model.skins.empty()) {
        err = "GLB contains no skinned mesh node";
        return false;
    }
    if (sel.skipped_other_skins > 0) {
        PTV_WARN(
            "{}: {} additional skin(s) ignored; only the first skin is "
            "loaded",
            path, sel.skipped_other_skins);
    }
    if (!build_skeleton(model, sel.skin, out_skeleton, err)) return false;

    const auto& skin = model.skins[static_cast<size_t>(sel.skin)];
    std::unordered_map<int, int> node_to_joint;
    for (size_t j = 0; j < skin.joints.size(); ++j) {
        node_to_joint[skin.joints[j]] = static_cast<int>(j);
    }
    parse_animations(model, node_to_joint, out_skeleton);

    const size_t joint_count = out_skeleton.joint_count();
    int skipped_static_prims = 0;

    for (int node : sel.mesh_nodes) {
        const int mesh_idx = model.nodes[static_cast<size_t>(node)].mesh;
        if (mesh_idx < 0 ||
            static_cast<size_t>(mesh_idx) >= model.meshes.size()) {
            continue;
        }
        for (const auto& prim :
             model.meshes[static_cast<size_t>(mesh_idx)].primitives) {
            if (!(prim.mode == TINYGLTF_MODE_TRIANGLES || prim.mode == -1))
                continue;

            auto pos_it = prim.attributes.find("POSITION");
            auto joints_it = prim.attributes.find("JOINTS_0");
            auto weights_it = prim.attributes.find("WEIGHTS_0");
            if (pos_it == prim.attributes.end()) continue;
            if (joints_it == prim.attributes.end() ||
                weights_it == prim.attributes.end()) {
                ++skipped_static_prims;
                continue;
            }

            std::vector<float> positions;
            size_t comps = 0;
            if (!read_accessor_floats(model, pos_it->second, positions, &comps,
                                      &err) ||
                comps != 3) {
                err = "POSITION accessor unreadable: " + err;
                return false;
            }
            const size_t vcount = positions.size() / 3;

            std::vector<float> normals;
            auto n_it = prim.attributes.find("NORMAL");
            if (n_it != prim.attributes.end()) {
                size_t nc = 0;
                std::string nerr;
                if (!read_accessor_floats(model, n_it->second, normals, &nc,
                                          &nerr) ||
                    nc != 3) {
                    normals.clear();
                }
            }

            std::vector<float> vert_colors;
            size_t color_comps = 0;
            auto c_it = prim.attributes.find("COLOR_0");
            if (c_it != prim.attributes.end()) {
                std::string cerr;
                if (!read_accessor_floats(model, c_it->second, vert_colors,
                                          &color_comps, &cerr)) {
                    vert_colors.clear();
                    color_comps = 0;
                } else {
                    mats.had_vertex_colors = true;
                }
            }

            const int mat_slot = gltf_materials::material_slot(mats, prim.material);
            const MaterialInfo& mat = mats.materials[static_cast<size_t>(mat_slot)];
            const auto& uv_xform = uv_xforms[static_cast<size_t>(mat_slot)];
            std::vector<float> uvs;
            auto uv_it = prim.attributes.find(
                "TEXCOORD_" + std::to_string(std::max(0, mat.base_color_texcoord)));
            if (uv_it != prim.attributes.end()) {
                size_t uc = 0;
                std::string uerr;
                if (!read_accessor_floats(model, uv_it->second, uvs, &uc, &uerr) ||
                    uc != 2) {
                    uvs.clear();
                }
            }
            if (mat.base_color_image >= 0 && uvs.size() < vcount * 2) {
                mats.missing_uvs = true;
            }

            std::vector<std::uint32_t> joints;
            size_t jc = 0;
            if (!read_accessor_uints(model, joints_it->second, joints, &jc,
                                     &err) ||
                jc != 4) {
                err = "JOINTS_0 accessor unreadable or not VEC4: " + err;
                return false;
            }
            std::vector<float> weights;
            size_t wc = 0;
            if (!read_accessor_floats(model, weights_it->second, weights, &wc,
                                      &err) ||
                wc != 4) {
                err = "WEIGHTS_0 accessor unreadable or not VEC4: " + err;
                return false;
            }
            if (joints.size() / 4 < vcount || weights.size() / 4 < vcount) {
                err = "JOINTS_0 / WEIGHTS_0 have fewer entries than POSITION";
                return false;
            }

            std::array<float, 4> mat_color{1.0f, 1.0f, 1.0f, 1.0f};
            if (options.bake_base_color_factor) {
                mat_color = material_base_color(model, prim.material);
            }

            const std::uint32_t base =
                static_cast<std::uint32_t>(out_verts.size());
            const size_t first_index = out_indices.size();
            for (size_t i = 0; i < vcount; ++i) {
                SkinnedVertex sv{};
                sv.pos[0] = positions[i * 3 + 0];
                sv.pos[1] = positions[i * 3 + 1];
                sv.pos[2] = positions[i * 3 + 2];
                if (i * 3 + 2 < normals.size()) {
                    sv.normal[0] = normals[i * 3 + 0];
                    sv.normal[1] = normals[i * 3 + 1];
                    sv.normal[2] = normals[i * 3 + 2];
                } else {
                    sv.normal[0] = 0.0f;
                    sv.normal[1] = 1.0f;
                    sv.normal[2] = 0.0f;
                }
                if (i * 2 + 1 < uvs.size()) {
                    float u = uvs[i * 2 + 0];
                    float v = uvs[i * 2 + 1];
                    uv_xform.apply(u, v);
                    sv.uv[0] = u;
                    sv.uv[1] = v;
                }
                for (int k = 0; k < 4; ++k) {
                    float cv = 1.0f;
                    if (color_comps >= 3 &&
                        i * color_comps + static_cast<size_t>(k) <
                            vert_colors.size() &&
                        static_cast<size_t>(k) < color_comps) {
                        cv = vert_colors[i * color_comps +
                                         static_cast<size_t>(k)];
                    }
                    sv.color[k] = cv * mat_color[static_cast<size_t>(k)];
                }

                float w[4];
                for (int k = 0; k < 4; ++k) {
                    const std::uint32_t ji = joints[i * 4 + static_cast<size_t>(k)];
                    if (ji >= joint_count) {
                        err = "JOINTS_0 references joint " +
                              std::to_string(ji) + " but the skin has only " +
                              std::to_string(joint_count) + " joints";
                        return false;
                    }
                    sv.joints[k] = static_cast<std::uint8_t>(ji);
                    w[k] = weights[i * 4 + static_cast<size_t>(k)];
                }
                quantize_weights(w, sv.weights);
                out_verts.push_back(sv);
            }

            if (prim.indices >= 0) {
                std::vector<std::uint32_t> idx;
                size_t ic = 0;
                if (!read_accessor_uints(model, prim.indices, idx, &ic, &err) ||
                    ic != 1) {
                    err = "index accessor unreadable: " + err;
                    return false;
                }
                for (std::uint32_t id : idx) out_indices.push_back(base + id);
            } else {
                if (vcount % 3 != 0) {
                    out_verts.resize(base);
                    continue;
                }
                for (size_t i = 0; i < vcount; i += 3) {
                    out_indices.push_back(base + static_cast<std::uint32_t>(i));
                    out_indices.push_back(base + static_cast<std::uint32_t>(i + 1));
                    out_indices.push_back(base + static_cast<std::uint32_t>(i + 2));
                }
            }
            const auto added =
                static_cast<std::uint32_t>(out_indices.size() - first_index);
            if (added > 0) {
                if (!mats.submeshes.empty() &&
                    mats.submeshes.back().material == mat_slot &&
                    mats.submeshes.back().first_index +
                            mats.submeshes.back().index_count ==
                        first_index) {
                    mats.submeshes.back().index_count += added;
                } else {
                    mats.submeshes.push_back(SubMesh{
                        static_cast<std::uint32_t>(first_index), added, mat_slot});
                }
            }
        }
    }

    if (skipped_static_prims > 0) {
        PTV_WARN("{}: skipped {} primitive(s) without JOINTS_0/WEIGHTS_0",
                 path, skipped_static_prims);
    }
    if (out_verts.empty() || out_indices.empty()) {
        err = "no skinned triangle geometry in GLB";
        return false;
    }

    bool need_normals = false;
    for (const auto& v : out_verts) {
        const float nlen =
            std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                      v.normal[2] * v.normal[2]);
        if (nlen < 1e-4f) {
            need_normals = true;
            break;
        }
    }
    if (need_normals) compute_vertex_normals(out_indices, out_verts);

    // Bounds of the bind pose AS DRAWN (palette applied), not of the raw
    // vertices: they differ whenever the inverse binds do not cancel the
    // skeleton's ancestors (see bind_palette_gain).
    std::vector<Mat4> bind_pal;
    {
        SkeletonData raw = out_skeleton;
        raw.unit_scale = 1.0f;
        build_joint_palette(raw, raw.bind_pose, bind_pal);
    }
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& v : out_verts) {
        Mat4 m(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            if (v.weights[k] == 0 || v.joints[k] >= bind_pal.size()) continue;
            const float w = static_cast<float>(v.weights[k]) / 255.0f;
            m += bind_pal[v.joints[k]] * w;
            wsum += w;
        }
        const Vec3 p = wsum > 1e-4f ? Vec3(m * Vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f))
                                    : Vec3(v.pos[0], v.pos[1], v.pos[2]);
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(p[k])) continue;
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    const float extent = std::max(hi[0] - lo[0],
                                  std::max(hi[1] - lo[1], hi[2] - lo[2]));
    if (extent < 1e-6f) {
        err = "degenerate bounds";
        return false;
    }
    const bool framed = frame != nullptr && frame->valid;
    if (framed) apply_frame_pivot(*frame, out_skeleton);
    const float scale =
        framed ? frame->scale_for(target_scale_meters)
               : ((target_scale_meters > 1e-4f) ? (target_scale_meters / extent)
                                                : 1.0f);
    if (scale != 1.0f) {
        for (auto& v : out_verts) {
            v.pos[0] *= scale;
            v.pos[1] *= scale;
            v.pos[2] *= scale;
        }
    }
    out_skeleton.unit_scale = scale;
    // ``lo`` is the drawn bind pose; a frame's pivot rotates about Y and so
    // leaves heights alone.
    out_skeleton.bind_min_y =
        lo[1] * scale -
        (framed ? (frame->sole_y + 0.5f * frame->height) * scale : 0.0f);

    int textured = 0;
    for (const auto& sm : mats.submeshes) {
        if (mats.materials[static_cast<size_t>(sm.material)].base_color_image >= 0) ++textured;
    }
    PTV_INFO(
        "skinned GLB loaded: {} verts, {} tris, {} joints, {} clip(s), "
        "{} submesh(es), {} textured, scale {:.3f} (target {:.2f} m){}",
        out_verts.size(), out_indices.size() / 3, joint_count,
        out_skeleton.clips.size(), mats.submeshes.size(), textured, scale,
        target_scale_meters,
        framed ? fmt::format(", model frame yaw {:.1f} deg, sole {:.3f}",
                             glm::degrees(frame->yaw_offset_rad), frame->sole_y)
               : std::string(", NO model frame (longest-axis scale)"));
    return true;
}

}  // namespace ptv::gltf_mesh_load
