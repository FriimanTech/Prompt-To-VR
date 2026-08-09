#include "ptv/scene/SceneLoader.h"

#include <fstream>
#include <sstream>

#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"

namespace ptv {

namespace {

Vec3 read_vec3(const Json& v, Vec3 fallback) {
    if (!v.is_array() || v.size() < 3) return fallback;
    return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
}

Quat read_quat(const Json& v) {
    if (!v.is_array() || v.size() < 4) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    return Quat(v[3].get<float>(), v[0].get<float>(), v[1].get<float>(),
                v[2].get<float>());  // glm::quat(w, x, y, z)
}

Transform read_transform(const Json& v) {
    Transform t{};
    if (!v.is_object()) return t;
    t.position = read_vec3(v.value("position", Json::array()), Vec3(0.0f));
    t.rotation = read_quat(v.value("rotation", Json::array()));
    t.scale = v.value("scale", 1.0f);
    return t;
}

}  // namespace

SceneDoc LoadSceneJson(const std::string& path) {
    // Read through the process-global asset source so scene.json loads from
    // the filesystem on desktop and from the APK (AAssetManager) on Quest.
    auto text = asset_source().read_text(path);
    if (!text) {
        PTV_ERROR("Cannot open scene file: {}", path);
        return {};
    }
    Json j;
    try {
        j = Json::parse(*text);
    } catch (const std::exception& e) {
        PTV_ERROR("Scene JSON parse error: {}", e.what());
        return {};
    }

    SceneDoc d{};
    d.version = j.value("version", 1);
    d.prompt_id = j.value("prompt_id", "");
    d.scene_id = j.value("scene_id", "");
    d.title = j.value("title", "");
    d.name = j.value("name", "");
    if (j.contains("player")) {
        const auto& p = j["player"];
        d.player_height_meters = p.value("height_meters", 1.7f);
        d.locomotion = p.value("locomotion", std::string("teleport"));
        d.comfort = p.value("comfort", std::string("comfort"));
        d.locomotion_menu_enabled = p.value("locomotion_menu", true);
        d.default_locomotion_mode = p.value("default_locomotion_mode", std::string{});
        d.slide_max_speed_mps = p.value("slide_max_speed_mps", 1.5f);
        d.slide_accel_tau_s = p.value("slide_accel_tau_s", 0.12f);
        d.slide_decel_tau_s = p.value("slide_decel_tau_s", 0.18f);
        d.teleport_launch_speed_mps = p.value("teleport_launch_speed_mps", 6.0f);
        d.teleport_max_horizontal_m = p.value("teleport_max_horizontal_m", 12.0f);
        d.teleport_floor_y = p.value("teleport_floor_y", 0.0f);
        d.snap_turn_degrees = p.value("snap_turn_degrees", 45.0f);
        d.snap_turn_cooldown_s = p.value("snap_turn_cooldown_s", 0.30f);
        if (p.contains("spawn")) {
            d.spawn.position = read_vec3(p["spawn"].value("position", Json::array()),
                                         Vec3(0.0f));
            d.spawn.rotation = read_quat(p["spawn"].value("rotation", Json::array()));
        }
    }
    d.sky_color = read_vec3(j.value("sky_color", Json::array()), d.sky_color);
    d.ambient_intensity = j.value("ambient_intensity", 0.25f);

    if (j.contains("sky") && j["sky"].is_object()) {
        const auto& s = j["sky"];
        d.sky.present = true;
        d.sky.top = read_vec3(s.value("top_color", Json::array()), d.sky.top);
        d.sky.horizon =
            read_vec3(s.value("horizon_color", Json::array()), d.sky.horizon);
        d.sky.sun_direction =
            read_vec3(s.value("sun_direction", Json::array()), d.sky.sun_direction);
        d.sky.sun_color =
            read_vec3(s.value("sun_color", Json::array()), d.sky.sun_color);
        d.sky.sun_intensity = s.value("sun_intensity", 1.0f);
        // Author-controlled environment knobs (default to the struct defaults
        // when an older world.scene.json omits them).
        d.sky.ambient_color =
            read_vec3(s.value("ambient_color", Json::array()), d.sky.ambient_color);
        d.sky.ambient_intensity =
            s.value("ambient_intensity", d.ambient_intensity);
        d.sky.fog_density = s.value("fog_density", d.sky.fog_density);
        d.sky.exposure = s.value("exposure", d.sky.exposure);
        // Keep the flat clear color in sync with the sky horizon.
        d.sky_color = d.sky.horizon;
    }

    // Device budget + quality policy (emitted by asset_optimization). Every
    // field falls back to the ScenePerformance defaults, so a scene compiled
    // before this stage existed still loads and simply runs with the
    // conservative built-in budget.
    if (j.contains("performance") && j["performance"].is_object()) {
        const auto& pf = j["performance"];
        auto& sp = d.performance;
        sp.tier = pf.value("tier", sp.tier);
        sp.target_fps = pf.value("target_fps", sp.target_fps);
        sp.frame_triangle_budget =
            pf.value("frame_triangle_budget", sp.frame_triangle_budget);
        sp.resident_triangle_budget =
            pf.value("resident_triangle_budget", sp.resident_triangle_budget);
        sp.draw_call_budget = pf.value("draw_call_budget", sp.draw_call_budget);
        sp.msaa_samples = pf.value("msaa_samples", sp.msaa_samples);
        sp.foveation_level = pf.value("foveation_level", sp.foveation_level);
        sp.cull_distance_m = pf.value("cull_distance_m", sp.cull_distance_m);
        sp.evict_when_over_budget =
            pf.value("evict_when_over_budget", sp.evict_when_over_budget);
        if (pf.contains("lod_switch_distances_m") &&
            pf["lod_switch_distances_m"].is_array()) {
            std::vector<float> dists;
            for (const auto& v : pf["lod_switch_distances_m"]) {
                if (v.is_number()) dists.push_back(v.get<float>());
            }
            if (!dists.empty()) sp.lod_switch_distances_m = std::move(dists);
        }
        if (pf.contains("governor") && pf["governor"].is_object()) {
            const auto& g = pf["governor"];
            sp.governor_enabled = g.value("enabled", sp.governor_enabled);
            sp.governor_max_lod_bias =
                g.value("max_lod_bias", sp.governor_max_lod_bias);
            sp.governor_min_cull_distance_m =
                g.value("min_cull_distance_m", sp.governor_min_cull_distance_m);
            sp.governor_max_foveation_level =
                g.value("max_foveation_level", sp.governor_max_foveation_level);
            sp.governor_escalate_after_bad_frames = g.value(
                "escalate_after_bad_frames", sp.governor_escalate_after_bad_frames);
            sp.governor_relax_after_good_frames = g.value(
                "relax_after_good_frames", sp.governor_relax_after_good_frames);
        }
        PTV_INFO(
            "Scene performance: tier={} budget={} tris/frame msaa={}x ffr={} "
            "cull={:.0f}m governor={}",
            sp.tier, sp.frame_triangle_budget, sp.msaa_samples,
            sp.foveation_level, sp.cull_distance_m,
            sp.governor_enabled ? "on" : "off");
    }

    for (const auto& l : j.value("lights", Json::array())) {
        LightSpec ls{};
        ls.id = l.value("id", "");
        ls.kind = l.value("kind", "directional");
        ls.transform = read_transform(l.value("transform", Json::object()));
        ls.color = read_vec3(l.value("color_rgb", Json::array()), Vec3(1.0f));
        ls.intensity = l.value("intensity", 1.0f);
        ls.direction =
            read_vec3(l.value("direction", Json::array()), Vec3(0.0f, -1.0f, 0.0f));
        ls.range = l.value("range", 0.0f);  // <= 0 -> renderer default
        d.lights.push_back(std::move(ls));
    }

    for (const auto& e : j.value("entities", Json::array())) {
        EntitySpec es{};
        es.id = e.value("id", "");
        es.name = e.value("name", "");
        es.transform = read_transform(e.value("transform", Json::object()));
        if (e.contains("mesh") && e["mesh"].is_object()) {
            MeshRef m{};
            m.asset_id = e["mesh"].value("asset_id", "");
            m.glb_path = e["mesh"].value("glb_path", "");
            m.target_scale_meters = e["mesh"].value("target_scale_meters", 1.0f);
            m.collision_shape = e["mesh"].value("collision_shape", "aabb");
            // Decimated LOD chain + culling radius from asset_optimization.
            // Absent on scenes compiled before that stage: the renderer then
            // just draws glb_path with a conservative radius.
            for (const auto& lp : e["mesh"].value("lod_paths", Json::array())) {
                if (lp.is_string()) m.lod_paths.push_back(lp.get<std::string>());
            }
            for (const auto& tc :
                 e["mesh"].value("lod_triangle_counts", Json::array())) {
                if (tc.is_number()) {
                    m.lod_triangle_counts.push_back(
                        tc.get<std::uint32_t>());
                }
            }
            m.bounds_radius_m = e["mesh"].value("bounds_radius_m", 0.0f);
            m.skinned = e["mesh"].value("skinned", false);
            m.joint_count = e["mesh"].value("joint_count", 0);
            const auto& anims = e["mesh"].value("animations", Json::object());
            if (anims.is_object()) {
                for (auto it = anims.begin(); it != anims.end(); ++it) {
                    if (!it.value().is_string()) continue;
                    m.animations.emplace(it.key(),
                                         it.value().get<std::string>());
                }
            }
            // Named attachment points, already in metres (see MeshRef).
            const auto& sockets = e["mesh"].value("sockets", Json::object());
            if (sockets.is_object()) {
                for (auto it = sockets.begin(); it != sockets.end(); ++it) {
                    if (!it.value().is_object()) continue;
                    m.sockets.emplace(it.key(), read_transform(it.value()));
                }
            }
            es.mesh = std::move(m);
        }
        if (e.contains("primitive") && e["primitive"].is_object()) {
            const auto& pj = e["primitive"];
            PrimitiveRef pr{};
            pr.kind = pj.value("kind", "box");
            pr.size = read_vec3(pj.value("size", Json::array()), Vec3(1.0f));
            pr.color =
                read_vec3(pj.value("color_rgb", Json::array()), Vec3(0.8f, 0.8f, 0.82f));
            es.primitive = std::move(pr);
        }
        if (e.contains("mesh_inline") && e["mesh_inline"].is_object()) {
            const auto& mi = e["mesh_inline"];
            InlineMeshRef im{};
            if (mi.contains("positions") && mi["positions"].is_array())
                im.positions = mi["positions"].get<std::vector<float>>();
            if (mi.contains("normals") && mi["normals"].is_array())
                im.normals = mi["normals"].get<std::vector<float>>();
            if (mi.contains("indices") && mi["indices"].is_array())
                im.indices = mi["indices"].get<std::vector<std::uint32_t>>();
            im.color =
                read_vec3(mi.value("color_rgb", Json::array()), Vec3(0.8f, 0.8f, 0.82f));
            es.mesh_inline = std::move(im);
        }
        if (e.contains("physics") && e["physics"].is_object()) {
            PhysicsProps p{};
            p.kind = e["physics"].value("kind", "static");
            p.mass_kg = e["physics"].value("mass_kg", 1.0f);
            p.collision_shape = e["physics"].value("collision_shape", "aabb");
            p.restitution = e["physics"].value("restitution", 0.2f);
            p.friction = e["physics"].value("friction", 0.5f);
            es.physics = std::move(p);
        }
        for (const auto& b : e.value("mechanic_bindings", Json::array())) {
            MechanicBinding mb{};
            mb.id = b.value("id", "");
            mb.builtin_id = b.value("builtin_id", "");
            mb.params = b.value("params", Json::object());
            es.bindings.push_back(std::move(mb));
        }
        for (const auto& t : e.value("tags", Json::array())) {
            es.tags.push_back(t.get<std::string>());
        }
        d.entities.push_back(std::move(es));
    }

    PTV_INFO("Loaded scene '{}' with {} entities, {} lights", d.scene_id,
             d.entities.size(), d.lights.size());
    return d;
}

void ApplySceneToWorld(const SceneDoc& doc, World& world) {
    for (const auto& es : doc.entities) {
        auto e = world.create_entity(es.id);
        world.registry().emplace<CTransform>(e, CTransform{es.transform});
        if (es.mesh.has_value()) {
            world.registry().emplace<CMesh>(
                e, CMesh{es.mesh->asset_id, es.mesh->glb_path,
                         es.mesh->target_scale_meters, es.mesh->lod_paths,
                         es.mesh->bounds_radius_m, es.mesh->sockets,
                         es.mesh->skinned, es.mesh->animations});
        }
        if (es.primitive.has_value()) {
            world.registry().emplace<CPrimitive>(
                e, CPrimitive{es.primitive->kind, es.primitive->size,
                              es.primitive->color});
        }
        if (es.mesh_inline.has_value()) {
            world.registry().emplace<CInlineMesh>(
                e, CInlineMesh{es.mesh_inline->positions, es.mesh_inline->normals,
                               es.mesh_inline->indices, es.mesh_inline->color});
        }
        if (es.physics.has_value()) {
            world.registry().emplace<CPhysics>(
                e,
                CPhysics{es.physics->kind, es.physics->mass_kg,
                         es.physics->restitution, es.physics->friction,
                         es.physics->collision_shape});
        }
        if (!es.bindings.empty()) {
            CMechanicBinding mb{};
            for (const auto& b : es.bindings) {
                mb.entries.push_back({b.id, b.builtin_id, b.params.dump()});
            }
            world.registry().emplace<CMechanicBinding>(e, std::move(mb));
        }
        if (!es.tags.empty()) {
            world.registry().emplace<CTag>(e, CTag{es.tags});
        }
    }
}

}  // namespace ptv
