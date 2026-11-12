// ptv_scene_views: offline renders of a compiled scene.json for scene_review.
//
// The shooting range compiled, built and ran -- with duplicate tables, guns
// beside the wrong table and lane-2 targets out on the floor. Every check
// that ran was about one thing at a time (an asset's sockets, an entity's
// support); nothing ever LOOKED at the layout the prompt described. This
// renders the whole scene from cameras the review stage chooses (the spawn,
// each requirement's viewpoint, an overview) with the same CPU rasteriser as
// ptv_glb_views, and writes next to each colour image an entity-id buffer, so
// "is the red button visible from where the player stands" is a pixel count
// rather than a guess.
//
//   ptv_scene_views --scene <scene.json> --root <dir> --cameras <cameras.json>
//                   --out-dir <dir> [--size 768]
//
// cameras.json: {"cameras": [{"id", "eye": [x,y,z], "target": [x,y,z],
//                             "fov_deg": 70, "max_y": <optional cutaway>}]}
// Writes <id>.png, <id>.ents (uint32 per pixel: entity index + 1) and
// views.json: entity ids, and per camera the visible pixel count of every
// entity, the entity under the centre pixel and the centre depth.
//
// Drawn: primitives (box / sphere / cylinder / plane / ramp, unit-size and
// scaled like the renderer), static GLB meshes (a mid LOD, node transforms x
// unit_scale, like the static loader) and skinned characters in bind pose
// centred on their model frame. Skipped: templates, hidden entities, the
// player rig and anything attached to another entity (it moves with it).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "cpu_raster.h"
#include "ptv/render/GltfMeshLoad.h"

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using ptv_raster::ClipVert;
using ptv_raster::Image;

struct Options {
    std::string scene, root, cameras, out_dir;
    int size = 768;
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--scene") {
            if (!next(o.scene)) return false;
        } else if (a == "--root") {
            if (!next(o.root)) return false;
        } else if (a == "--cameras") {
            if (!next(o.cameras)) return false;
        } else if (a == "--out-dir") {
            if (!next(o.out_dir)) return false;
        } else if (a == "--size") {
            if (!next(v)) return false;
            o.size = std::clamp(std::stoi(v), 128, 2048);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return !o.scene.empty() && !o.cameras.empty() && !o.out_dir.empty();
}

glm::vec3 vec3_of(const json& j, glm::vec3 fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    for (int i = 0; i < 3; ++i)
        if (!j[static_cast<size_t>(i)].is_number()) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

glm::quat quat_of(const json& j) {
    if (!j.is_array() || j.size() < 4) return glm::quat(1, 0, 0, 0);
    const glm::quat q(j[3].get<float>(), j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    return glm::length(q) > 1e-6f ? glm::normalize(q) : glm::quat(1, 0, 0, 0);
}

/** A triangle soup in local space, with an optional texture per triangle. */
struct Soup {
    std::vector<glm::vec3> pos, nrm;
    std::vector<glm::vec2> uv;
    std::vector<glm::vec4> col;
    std::vector<uint32_t> idx;
    std::vector<int> tri_image;
    std::vector<Image> images;
    glm::vec3 lo{0.0f}, hi{0.0f};
    bool ok = false;
};

void quad(Soup& s, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, glm::vec3 n) {
    const auto base = static_cast<uint32_t>(s.pos.size());
    for (const auto& p : {a, b, c, d}) {
        s.pos.push_back(p);
        s.nrm.push_back(n);
        s.uv.push_back(glm::vec2(0.0f));
        s.col.push_back(glm::vec4(1.0f));
    }
    s.idx.insert(s.idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    s.tri_image.push_back(-1);
    s.tri_image.push_back(-1);
}

Soup unit_primitive(const std::string& kind) {
    Soup s;
    const float h = 0.5f;
    if (kind == "sphere" || kind == "cylinder") {
        const int seg = 20, stacks = kind == "sphere" ? 10 : 1;
        for (int i = 0; i < stacks; ++i) {
            for (int j = 0; j < seg; ++j) {
                auto at = [&](int ii, int jj) {
                    const float t = 6.2831853f * static_cast<float>(jj) / seg;
                    if (kind == "cylinder") {
                        return glm::vec3(h * std::cos(t), ii == 0 ? -h : h, h * std::sin(t));
                    }
                    const float phi = 3.14159265f * static_cast<float>(ii) / stacks;
                    return glm::vec3(h * std::sin(phi) * std::cos(t), h * std::cos(phi),
                                     h * std::sin(phi) * std::sin(t));
                };
                const glm::vec3 a = at(i, j), b = at(i, j + 1), c = at(i + 1, j + 1), d = at(i + 1, j);
                glm::vec3 n = (a + b + c + d) * 0.25f;
                if (kind == "cylinder") n.y = 0.0f;
                quad(s, a, b, c, d, glm::normalize(n));
            }
        }
        if (kind == "cylinder") {
            for (int j = 0; j < seg; ++j) {
                const float t0 = 6.2831853f * static_cast<float>(j) / seg;
                const float t1 = 6.2831853f * static_cast<float>(j + 1) / seg;
                for (const float y : {-h, h}) {
                    const glm::vec3 c(0, y, 0), a(h * std::cos(t0), y, h * std::sin(t0)),
                        b(h * std::cos(t1), y, h * std::sin(t1));
                    quad(s, c, a, b, c, glm::vec3(0, y > 0 ? 1.0f : -1.0f, 0));
                }
            }
        }
    } else if (kind == "plane") {
        quad(s, {-h, 0, -h}, {-h, 0, h}, {h, 0, h}, {h, 0, -h}, {0, 1, 0});
    } else if (kind == "ramp") {
        const glm::vec3 L0{-h, -h, -h}, L1{-h, -h, h}, L2{-h, h, h};
        const glm::vec3 R0{h, -h, -h}, R1{h, -h, h}, R2{h, h, h};
        quad(s, L0, R0, R1, L1, {0, -1, 0});
        quad(s, L1, R1, R2, L2, {0, 0, 1});
        quad(s, L2, R2, R0, L0, glm::normalize(glm::cross(R2 - L2, R0 - L2)));
        quad(s, L0, L1, L2, L2, {-1, 0, 0});
        quad(s, R0, R2, R1, R1, {1, 0, 0});
    } else {
        quad(s, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, {1, 0, 0});
        quad(s, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}, {-1, 0, 0});
        quad(s, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, {0, 1, 0});
        quad(s, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {0, -1, 0});
        quad(s, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
        quad(s, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
    }
    s.ok = true;
    return s;
}

Soup load_glb(const std::string& path) {
    Soup s;
    ptv::gltf_mesh_load::TexturedMesh mesh;
    std::string err;
    if (!ptv::gltf_mesh_load::load_glb_textured(path, mesh, err) || mesh.verts.empty()) {
        std::fprintf(stderr, "skip %s: %s\n", path.c_str(), err.c_str());
        return s;
    }
    s.lo = glm::vec3(1e30f);
    s.hi = glm::vec3(-1e30f);
    for (const auto& v : mesh.verts) {
        const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
        s.pos.push_back(p);
        s.nrm.push_back(glm::vec3(v.normal[0], v.normal[1], v.normal[2]));
        s.uv.push_back(glm::vec2(v.uv[0], v.uv[1]));
        s.col.push_back(glm::vec4(v.color[0], v.color[1], v.color[2], v.color[3]));
        s.lo = glm::min(s.lo, p);
        s.hi = glm::max(s.hi, p);
    }
    s.idx = mesh.indices;
    s.tri_image = mesh.tri_image;
    s.images.resize(mesh.images.size());
    for (size_t i = 0; i < mesh.images.size(); ++i) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(
            mesh.images[i].bytes.data(), static_cast<int>(mesh.images[i].bytes.size()), &w, &h,
            &n, 4);
        if (px != nullptr) {
            s.images[i].w = w;
            s.images[i].h = h;
            s.images[i].rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
        }
    }
    s.ok = true;
    return s;
}

bool has_tag(const json& e, const char* tag) {
    if (!e.contains("tags") || !e["tags"].is_array()) return false;
    for (const auto& t : e["tags"])
        if (t.is_string() && t.get<std::string>() == tag) return true;
    return false;
}

/** One thing to draw: a soup and the matrix taking it to world space. */
struct Draw {
    const Soup* soup = nullptr;
    glm::mat4 model{1.0f};
    glm::vec4 tint{1.0f};
    uint32_t entity = 0;  // index + 1
};

std::string pick_mesh_path(const json& mesh) {
    if (mesh.contains("lod_paths") && mesh["lod_paths"].is_array() && !mesh["lod_paths"].empty()) {
        const auto& lods = mesh["lod_paths"];
        const size_t k = std::min<size_t>(1, lods.size() - 1);
        if (lods[k].is_string()) return lods[k].get<std::string>();
    }
    if (mesh.contains("glb_path") && mesh["glb_path"].is_string())
        return mesh["glb_path"].get<std::string>();
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fprintf(stderr,
                     "usage: ptv_scene_views --scene <scene.json> --root <dir> "
                     "--cameras <cameras.json> --out-dir <dir> [--size 768]\n");
        return 2;
    }
    json scene, cams;
    try {
        std::ifstream(opt.scene) >> scene;
        std::ifstream(opt.cameras) >> cams;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bad json: %s\n", e.what());
        return 1;
    }
    const fs::path root = opt.root.empty() ? fs::current_path() : fs::path(opt.root);

    std::map<std::string, Soup> prims;
    std::unordered_map<std::string, Soup> meshes;
    std::vector<Draw> draws;
    std::vector<std::string> ids;
    const json& ents = scene.value("entities", json::array());
    for (size_t i = 0; i < ents.size(); ++i) {
        const json& e = ents[i];
        ids.push_back(e.value("id", std::string()));
        if (!e.is_object()) continue;
        if (has_tag(e, "template") || has_tag(e, "hidden") || has_tag(e, "player_rig") ||
            has_tag(e, "player_system") || has_tag(e, "player_hand") ||
            has_tag(e, "player_avatar"))
            continue;
        if (e.contains("attached_to") && !e["attached_to"].is_null()) continue;
        const json tj = e.value("transform", json::object());
        const glm::vec3 pos = vec3_of(tj.value("position", json()), glm::vec3(0.0f));
        const glm::quat rot = quat_of(tj.value("rotation", json()));
        glm::vec3 scale(1.0f);
        if (tj.contains("scale")) {
            if (tj["scale"].is_number()) scale = glm::vec3(tj["scale"].get<float>());
            else scale = vec3_of(tj["scale"], glm::vec3(1.0f));
        }
        const glm::mat4 entity_m = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot) *
                                   glm::scale(glm::mat4(1.0f), scale);
        Draw d;
        d.entity = static_cast<uint32_t>(i + 1);
        if (e.contains("primitive") && e["primitive"].is_object()) {
            const json& p = e["primitive"];
            const std::string kind = p.value("kind", std::string("box"));
            auto it = prims.find(kind);
            if (it == prims.end()) it = prims.emplace(kind, unit_primitive(kind)).first;
            d.soup = &it->second;
            d.model = entity_m * glm::scale(glm::mat4(1.0f),
                                            vec3_of(p.value("size", json()), glm::vec3(1.0f)));
            d.tint = glm::vec4(vec3_of(p.value("color_rgb", json()), glm::vec3(0.6f)), 1.0f);
            draws.push_back(d);
            continue;
        }
        if (!e.contains("mesh") || !e["mesh"].is_object()) continue;
        const json& m = e["mesh"];
        const std::string rel = pick_mesh_path(m);
        if (rel.empty()) continue;
        fs::path path(rel);
        if (path.is_relative()) path = root / path;
        auto it = meshes.find(path.string());
        if (it == meshes.end()) it = meshes.emplace(path.string(), load_glb(path.string())).first;
        if (!it->second.ok) continue;
        const Soup& s = it->second;
        float unit = m.value("unit_scale", 0.0f);
        if (unit <= 1e-8f) {
            const glm::vec3 ext = s.hi - s.lo;
            const float longest = std::max({ext.x, ext.y, ext.z});
            unit = longest > 1e-8f ? m.value("target_scale_meters", 1.0f) / longest : 1.0f;
        }
        glm::mat4 local = glm::scale(glm::mat4(1.0f), glm::vec3(unit));
        if (m.value("skinned", false)) {
            // Bind pose centred on the model frame: the runtime pivots the
            // character so its origin sits at its middle, soles foot_offset
            // below, facing the entity's forward.
            const glm::vec3 c = (s.lo + s.hi) * 0.5f * unit;
            const float sole = (s.lo.y) * unit;
            const float foot = m.value("foot_offset_m", 0.5f * (s.hi.y - s.lo.y) * unit);
            float yaw = 0.0f;
            if (m.contains("model_frame") && m["model_frame"].is_object())
                yaw = m["model_frame"].value("yaw_offset_rad", 0.0f);
            local = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0)) *
                    glm::translate(glm::mat4(1.0f), glm::vec3(-c.x, -sole - foot, -c.z)) * local;
        }
        d.soup = &s;
        d.model = entity_m * local;
        draws.push_back(d);
    }

    const glm::vec3 sky = vec3_of(scene.value("sky_color", json()), glm::vec3(0.62f, 0.72f, 0.84f));
    fs::create_directories(opt.out_dir);
    json out_views = json::array();
    const json& cam_list = cams.value("cameras", json::array());
    for (const auto& cj : cam_list) {
        const std::string cid = cj.value("id", std::string("view"));
        ptv_raster::Camera cam;
        cam.ortho = false;
        cam.eye = vec3_of(cj.value("eye", json()), glm::vec3(0, 1.6f, 0));
        const glm::vec3 target = vec3_of(cj.value("target", json()), cam.eye + glm::vec3(0, 0, -1));
        cam.forward = glm::normalize(target - cam.eye);
        glm::vec3 up(0, 1, 0);
        if (std::fabs(glm::dot(up, cam.forward)) > 0.98f) up = glm::vec3(0, 0, -1);
        cam.up = glm::normalize(up - cam.forward * glm::dot(up, cam.forward));
        cam.view = glm::lookAtRH(cam.eye, target, cam.up);
        const float fov = glm::radians(std::clamp(cj.value("fov_deg", 70.0f), 10.0f, 120.0f));
        cam.proj = glm::perspectiveRH_ZO(fov, 1.0f, 0.05f, cj.value("far_m", 250.0f));
        const glm::mat4 vp = cam.proj * cam.view;

        // A cutaway: an overview from above sees through the roof.
        const bool cut = cj.contains("max_y") && cj["max_y"].is_number();
        const float max_y = cut ? cj["max_y"].get<float>() : 0.0f;
        ptv_raster::Target target_buf(opt.size, sky);
        for (const Draw& d : draws) {
            const Soup& s = *d.soup;
            const glm::mat3 nmat = glm::transpose(glm::inverse(glm::mat3(d.model)));
            for (size_t t = 0; t + 2 < s.idx.size(); t += 3) {
                ClipVert cv[3];
                float lowest = 1e30f;
                for (int k = 0; k < 3; ++k) {
                    const uint32_t vi = s.idx[t + k];
                    const glm::vec3 wp = glm::vec3(d.model * glm::vec4(s.pos[vi], 1.0f));
                    lowest = std::min(lowest, wp.y);
                    cv[k] = ptv_raster::make_vert(vp, wp, nmat * s.nrm[vi], s.uv[vi],
                                                  s.col[vi] * d.tint);
                }
                if (cut && lowest > max_y) continue;
                const size_t tri = t / 3;
                const int img = tri < s.tri_image.size() ? s.tri_image[tri] : -1;
                const Image* tex = (img >= 0 && static_cast<size_t>(img) < s.images.size() &&
                                    s.images[static_cast<size_t>(img)].w > 0)
                                       ? &s.images[static_cast<size_t>(img)]
                                       : nullptr;
                ptv_raster::raster_clipped(target_buf, cam, cv, tex, d.entity, true, nullptr);
            }
        }
        const fs::path png = fs::path(opt.out_dir) / (cid + ".png");
        if (!stbi_write_png(png.string().c_str(), opt.size, opt.size, 3, target_buf.rgb.data(),
                            opt.size * 3)) {
            std::fprintf(stderr, "cannot write %s\n", png.string().c_str());
            return 1;
        }
        std::ofstream(fs::path(opt.out_dir) / (cid + ".ents"), std::ios::binary)
            .write(reinterpret_cast<const char*>(target_buf.ids.data()),
                   static_cast<std::streamsize>(target_buf.ids.size() * sizeof(uint32_t)));
        std::map<uint32_t, int> counts;
        for (const uint32_t id : target_buf.ids)
            if (id != 0) ++counts[id];
        json visible = json::object();
        for (const auto& [id, n] : counts) visible[ids[id - 1]] = n;
        const size_t centre = static_cast<size_t>(opt.size / 2) * opt.size + opt.size / 2;
        const uint32_t cid_ent = target_buf.ids[centre];
        out_views.push_back({{"id", cid},
                             {"image", cid + ".png"},
                             {"size", opt.size},
                             {"visible", visible},
                             {"centre_entity", cid_ent ? json(ids[cid_ent - 1]) : json()},
                             {"centre_depth_m", cid_ent ? json(target_buf.view_depth[centre]) : json()}});
    }
    json doc;
    doc["scene"] = opt.scene;
    doc["entities"] = ids;
    doc["drawn"] = draws.size();
    doc["views"] = out_views;
    std::ofstream(fs::path(opt.out_dir) / "views.json") << doc.dump(2) << "\n";
    std::printf("rendered %zu view(s) of %zu entities\n", out_views.size(), draws.size());
    return 0;
}
