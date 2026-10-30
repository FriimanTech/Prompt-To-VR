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

/** Parse a tagged state value: ``{"type": "...", "value": ...}``.
 *
 *  The type tag is not redundant. An enumerated position called "3" and the
 *  number 3 are different states, and a reader that guessed from the JSON
 *  type alone would silently conflate them. */
bool read_interactive_value(const Json& j, InteractiveValue* out) {
    if (out == nullptr || !j.is_object()) return false;
    const std::string type = j.value("type", std::string());
    if (!j.contains("value")) return false;
    const auto& v = j["value"];
    if (type == "bool" && v.is_boolean()) {
        *out = InteractiveValue::of(v.get<bool>());
    } else if (type == "number" && v.is_number()) {
        *out = InteractiveValue::of(v.get<float>());
    } else if (type == "name" && v.is_string()) {
        *out = InteractiveValue::named(v.get<std::string>());
    } else if (type == "vector") {
        if (!v.is_array() || v.size() < 3) return false;
        *out = InteractiveValue::of(read_vec3(v, Vec3(0.0f)));
    } else {
        return false;
    }
    return true;
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
        d.locomotion_menu_enabled = p.value("locomotion_menu", false);
        d.default_locomotion_mode = p.value("default_locomotion_mode", std::string{});
        d.default_turn_mode = p.value("default_turn_mode", std::string("snap"));
        if (p.contains("hands") && p["hands"].is_object()) {
            const auto& hj = p["hands"];
            d.hands_source = hj.value("source", std::string("default"));
            d.hands_scale_m =
                std::clamp(hj.value("scale_m", 0.20f), 0.08f, 0.5f);
            d.hands_grip_trim_deg =
                read_vec3(hj.value("grip_trim_deg", Json::array()), Vec3(0.0f));
            d.hands_debug_axes = hj.value("debug_axes", false);
        }
        d.slide_max_speed_mps = p.value("slide_max_speed_mps", 1.5f);
        d.slide_accel_tau_s = p.value("slide_accel_tau_s", 0.12f);
        d.slide_decel_tau_s = p.value("slide_decel_tau_s", 0.18f);
        d.sprint_speed_mps = std::clamp(
            p.value("sprint_speed_mps", 3.0f),
            std::max(0.2f, d.slide_max_speed_mps), 8.0f);
        if (p.contains("sprint") && p["sprint"].is_object()) {
            const auto& sj = p["sprint"];
            std::string act = sj.value("activation", std::string("hold"));
            if (!sj.value("enabled", true)) act = "off";
            if (act != "hold" && act != "toggle" && act != "off") act = "hold";
            d.sprint_activation = act;
            d.sprint_accel_tau_s =
                std::clamp(sj.value("accel_tau_s", 0.35f), 0.02f, 3.0f);
            d.sprint_decel_tau_s =
                std::clamp(sj.value("decel_tau_s", 0.25f), 0.02f, 3.0f);
            d.sprint_forward_cone_deg =
                std::clamp(sj.value("forward_cone_deg", 70.0f), 10.0f, 180.0f);
            d.sprint_min_stick =
                std::clamp(sj.value("min_stick", 0.6f), 0.1f, 1.0f);
        }
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
        sp.max_animated_agents =
            pf.value("max_animated_agents", sp.max_animated_agents);
        sp.max_live_projectiles = pf.value("max_live_projectiles", sp.max_live_projectiles);
        sp.max_projectile_meshes =
            pf.value("max_projectile_meshes", sp.max_projectile_meshes);
        sp.max_fx_quads = pf.value("max_fx_quads", sp.max_fx_quads);
        sp.max_beams = pf.value("max_beams", sp.max_beams);
        sp.max_decals = pf.value("max_decals", sp.max_decals);
        sp.max_flash_lights = pf.value("max_flash_lights", sp.max_flash_lights);
        auto read_float_list = [&pf](const char* key,
                                     std::vector<float>& out) {
            if (!pf.contains(key) || !pf[key].is_array()) return;
            std::vector<float> vals;
            for (const auto& v : pf[key]) {
                if (v.is_number()) vals.push_back(v.get<float>());
            }
            if (!vals.empty()) out = std::move(vals);
        };
        read_float_list("animation_lod_distances_m",
                        sp.animation_lod_distances_m);
        read_float_list("ai_tick_rates_hz", sp.ai_tick_rates_hz);
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

    for (const auto& pj : j.value("projectiles", Json::array())) {
        if (!pj.is_object()) continue;
        ProjectileTableEntry pe{};
        pe.id = pj.value("id", "");
        if (pe.id.empty()) continue;
        pe.kind = pj.value("kind", "bullet");
        pe.template_entity = pj.value("template", "");
        pe.speed_mps = pj.value("speed_mps", pe.speed_mps);
        pe.gravity_scale = pj.value("gravity_scale", pe.gravity_scale);
        pe.drag = pj.value("drag", pe.drag);
        pe.radius_m = pj.value("radius_m", pe.radius_m);
        pe.lifetime_s = pj.value("lifetime_s", pe.lifetime_s);
        pe.on_hit = pj.value("on_hit", pe.on_hit);
        pe.splash_radius_m = pj.value("splash_radius_m", pe.splash_radius_m);
        pe.splash_damage = pj.value("splash_damage", pe.splash_damage);
        pe.diameter_m = pj.value("diameter_m", pe.diameter_m);
        pe.length_m = pj.value("length_m", pe.length_m);
        d.projectiles.push_back(std::move(pe));
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

    if (j.contains("navigation") && j["navigation"].is_object()) {
        const auto& nav = j["navigation"];
        for (const auto& r : nav.value("regions", Json::array())) {
            if (!r.is_object()) continue;
            NavRegionSpec ns{};
            ns.id = r.value("id", "");
            if (ns.id.empty()) continue;
            ns.min = read_vec3(r.value("min", Json::array()), Vec3(0.0f));
            ns.max = read_vec3(r.value("max", Json::array()), Vec3(0.0f));
            ns.floor_y = r.value("floor_y", 0.0f);
            d.nav_regions.push_back(std::move(ns));
        }
        for (const auto& l : nav.value("links", Json::array())) {
            if (!l.is_object()) continue;
            NavLinkSpec ls{};
            ls.from = l.value("from", "");
            ls.to = l.value("to", "");
            if (ls.from.empty() || ls.to.empty()) continue;
            ls.point = read_vec3(l.value("point", Json::array()), Vec3(0.0f));
            ls.cost_multiplier = l.value("cost_multiplier", 1.0f);
            ls.bidirectional = l.value("bidirectional", true);
            d.nav_links.push_back(std::move(ls));
        }
    }

    for (const auto& i : j.value("interactives", Json::array())) {
        if (!i.is_object()) continue;
        InteractiveSpec is{};
        is.id = i.value("id", "");
        // An interactive with no id is unusable: the id IS its persistence
        // key, so state written against an empty one would collide with
        // every other unnamed object.
        if (is.id.empty()) continue;
        is.kind = i.value("kind", "");
        is.locked = i.value("locked", false);
        for (const auto& t : i.value("tags", Json::array())) {
            if (t.is_string()) is.tags.push_back(t.get<std::string>());
        }
        if (i.contains("initial_state") && i["initial_state"].is_object()) {
            for (const auto& [key, value] : i["initial_state"].items()) {
                InteractiveValue parsed{};
                if (read_interactive_value(value, &parsed)) {
                    is.initial_state[key] = parsed;
                }
            }
        }
        is.hinge_point =
            read_vec3(i.value("hinge_point", Json::array()), Vec3(0.0f));
        is.hinge_axis = read_vec3(i.value("hinge_axis", Json::array()),
                                  Vec3(0.0f, 1.0f, 0.0f));
        is.min_angle_deg = i.value("min_angle_deg", 0.0f);
        is.max_angle_deg = i.value("max_angle_deg", 90.0f);
        is.slide_axis =
            read_vec3(i.value("slide_axis", Json::array()), Vec3(0.0f));
        is.slide_distance_m = i.value("slide_distance_m", 0.0f);
        for (const auto& p : i.value("positions", Json::array())) {
            if (p.is_string()) is.positions.push_back(p.get<std::string>());
        }
        is.yields_asset_id = i.value("yields_asset_id", "");
        d.interactives.push_back(std::move(is));
    }

    for (const auto& sp : j.value("spawn_points", Json::array())) {
        if (!sp.is_object()) continue;
        SpawnPointSpec s{};
        s.id = sp.value("id", "");
        // An unnamed arrival point cannot be arrived AT -- a portal names the
        // spawn it targets, so a blank id is unreachable by construction.
        if (s.id.empty()) continue;
        s.position = read_vec3(sp.value("position", Json::array()), Vec3(0.0f));
        s.facing = read_vec3(sp.value("facing", Json::array()),
                             Vec3(0.0f, 0.0f, -1.0f));
        d.spawn_points.push_back(std::move(s));
    }

    for (const auto& p : j.value("portals", Json::array())) {
        if (!p.is_object()) continue;
        PortalSpec ps{};
        ps.id = p.value("id", "");
        ps.to_scene = p.value("to_scene", "");
        // A portal with no destination is scenery. Dropping it here rather
        // than at the point of use means the frame loop never has to test for
        // it, and the load says once that it was ignored.
        if (ps.id.empty() || ps.to_scene.empty()) {
            PTV_WARN("scene: portal '{}' has no destination; ignoring",
                     ps.id.empty() ? "(unnamed)" : ps.id);
            continue;
        }
        ps.target_spawn_id = p.value("target_spawn_id", "");
        if (ps.target_spawn_id.empty()) {
            ps.target_spawn_id = p.value("spawn_point_id", "");
        }
        ps.position = read_vec3(p.value("position", Json::array()), Vec3(0.0f));
        ps.size = read_vec3(p.value("size", Json::array()),
                            Vec3(1.0f, 2.0f, 0.5f));
        ps.requires_interaction = p.value("requires_interaction", true);
        d.portals.push_back(std::move(ps));
    }

    const auto& index = j.value("scene_index", Json::object());
    if (index.is_object()) {
        for (auto it = index.begin(); it != index.end(); ++it) {
            if (it.value().is_string()) {
                d.scene_index.emplace(it.key(), it.value().get<std::string>());
            }
        }
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
            m.unit_scale = std::max(0.0f, e["mesh"].value("unit_scale", 0.0f));
            m.foot_offset_m =
                std::clamp(e["mesh"].value("foot_offset_m", 0.0f), 0.0f, 100.0f);
            const auto& frame = e["mesh"].value("model_frame", Json::object());
            if (frame.is_object() && frame.value("height", 0.0f) > 1e-6f) {
                m.has_model_frame = true;
                m.model_height = frame.value("height", 0.0f);
                m.model_sole_y = frame.value("sole_y", 0.0f);
                m.model_yaw_offset_rad = frame.value("yaw_offset_rad", 0.0f);
            }
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
            // How fast each locomotion clip looks like it is moving, so the
            // runtime can match playback rate to ground covered instead of
            // letting the feet skate.
            const auto& speeds =
                e["mesh"].value("animation_speeds", Json::object());
            if (speeds.is_object()) {
                for (auto it = speeds.begin(); it != speeds.end(); ++it) {
                    if (!it.value().is_number()) continue;
                    m.animation_speeds.emplace(it.key(),
                                               it.value().get<float>());
                }
            }
            // Named attachment points, already in metres (see MeshRef).
            const auto& sockets = e["mesh"].value("sockets", Json::object());
            if (sockets.is_object()) {
                for (auto it = sockets.begin(); it != sockets.end(); ++it) {
                    if (!it.value().is_object()) continue;
                    m.sockets.emplace(it.key(), read_transform(it.value()));
                    const auto& sj = it.value();
                    if (sj.contains("kind")) {
                        SocketInfoSpec si{};
                        si.kind = sj.value("kind", "");
                        si.mirror = sj.value("mirror", "");
                        si.wrap_of = sj.value("wrap_of", "");
                        si.lateral_offset_m = sj.value("lateral_offset_m", 0.0f);
                        si.bore_diameter_m = sj.value("bore_diameter_m", 0.0f);
                        si.handle_thickness_m = sj.value("handle_thickness_m", 0.0f);
                        si.confidence = sj.value("confidence", "");
                        m.socket_info.emplace(it.key(), std::move(si));
                    }
                }
            }
            if (e["mesh"].contains("hold") && e["mesh"]["hold"].is_object()) {
                const auto& hj = e["mesh"]["hold"];
                HoldSpec h{};
                h.kind = hj.value("kind", "none");
                h.aim_aligned = hj.value("aim_aligned", false);
                h.two_hand = hj.value("two_hand", "none");
                h.primary_socket = hj.value("primary_socket", "");
                h.support_socket = hj.value("support_socket", "");
                h.forearm_socket = hj.value("forearm_socket", "");
                h.finger_pose = hj.value("finger_pose", "wrap");
                if (hj.contains("forward") && hj["forward"].is_array()) {
                    h.has_forward = true;
                    h.forward = read_vec3(hj["forward"], Vec3(0.0f, 0.0f, 1.0f));
                    h.up = read_vec3(hj.value("up", Json::array()), Vec3(0.0f, 1.0f, 0.0f));
                }
                if (!h.primary_socket.empty()) m.hold = std::move(h);
            }
            es.mesh = std::move(m);
        }
        if (e.contains("health") && e["health"].is_object()) {
            HealthProps h{};
            h.max = e["health"].value("max", 100.0f);
            h.invuln_after_hit_s =
                e["health"].value("invuln_after_hit_s", 0.4f);
            es.health = h;
        }
        if (e.contains("faction") && e["faction"].is_object()) {
            FactionProps f{};
            f.id = e["faction"].value("id", "neutral");
            for (const auto& h :
                 e["faction"].value("hostile_to", Json::array())) {
                if (h.is_string()) f.hostile_to.push_back(h.get<std::string>());
            }
            es.faction = std::move(f);
        }
        if (e.contains("attached_to") && e["attached_to"].is_object()) {
            const auto& aj = e["attached_to"];
            AttachmentRef ar{};
            ar.entity_id = aj.value("entity_id", "");
            ar.anchor = aj.value("anchor", "grip");
            ar.position = read_vec3(aj.value("position", Json::array()),
                                    Vec3(0.0f));
            ar.rotation = read_quat(aj.value("rotation", Json::array()));
            ar.socket = aj.value("socket", "");
            if (!ar.entity_id.empty()) es.attached_to = std::move(ar);
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
        // Places items may be equipped ON this entity. Declared on the
        // wearer, so a holster prop carries none of these and the player rig
        // carries them all.
        for (const auto& sj : e.value("equip_slots", Json::array())) {
            if (!sj.is_object()) continue;
            EquipSlotSpec slot{};
            slot.name = sj.value("name", "");
            if (slot.name.empty()) continue;  // unaddressable; drop it
            slot.body_slot = sj.value("body_slot", "");
            slot.host_item_slot = sj.value("host_item_slot", "");
            slot.host_socket = sj.value("host_socket", "");
            slot.local_position =
                read_vec3(sj.value("local_position", Json::array()), Vec3(0.0f));
            slot.local_rotation = read_quat(sj.value("local_rotation", Json::array()));
            slot.snap_radius_m = sj.value("snap_radius_m", 0.0f);
            slot.exclusive = sj.value("exclusive", true);
            for (const auto& t : sj.value("accepts_tags", Json::array())) {
                if (t.is_string()) slot.accepts_tags.push_back(t.get<std::string>());
            }
            es.equip_slots.push_back(std::move(slot));
        }
        d.entities.push_back(std::move(es));
    }

    PTV_INFO("Loaded scene '{}' with {} entities, {} lights", d.scene_id,
             d.entities.size(), d.lights.size());
    return d;
}

void ApplySceneToWorld(const SceneDoc& doc, World& world,
                       const SceneNesting* nesting) {
    // Ids are namespaced and transforms re-based when nesting. Both are
    // resolved through these two lambdas so every id lookup later in the
    // function (attachments, interactives, nav) goes through the same mapping
    // -- an entity created under a prefix but referenced without one would
    // resolve to nothing, silently.
    auto scoped = [&nesting](const std::string& id) {
        if (nesting == nullptr || nesting->id_prefix.empty() || id.empty()) {
            return id;
        }
        return nesting->id_prefix + "/" + id;
    };
    auto placed = [&nesting](const Transform& t) {
        if (nesting == nullptr) return t;
        const Mat4 world_m = nesting->mount * t.matrix();
        Transform out = t;
        out.position = Vec3(world_m[3]);
        out.rotation = glm::normalize(glm::quat_cast(Mat4(glm::mat3(
            glm::normalize(Vec3(world_m[0])), glm::normalize(Vec3(world_m[1])),
            glm::normalize(Vec3(world_m[2]))))));
        // Scale is carried on the Transform, not left in the matrix: the
        // attachment system that moves the interior each frame rewrites
        // position and rotation but never scale, so anything left in the
        // matrix would be lost on the first frame.
        out.scale = t.scale * nesting->scale;
        return out;
    };

    for (const auto& es : doc.entities) {
        auto e = world.create_entity(scoped(es.id));
        world.registry().emplace<CTransform>(e,
                                             CTransform{placed(es.transform)});
        if (es.mesh.has_value()) {
            world.registry().emplace<CMesh>(
                e, CMesh{es.mesh->asset_id, es.mesh->glb_path,
                         es.mesh->target_scale_meters, es.mesh->lod_paths,
                         es.mesh->bounds_radius_m, es.mesh->sockets,
                         es.mesh->skinned, es.mesh->animations,
                         es.mesh->animation_speeds, es.mesh->unit_scale,
                         es.mesh->foot_offset_m, es.mesh->has_model_frame,
                         es.mesh->model_yaw_offset_rad, es.mesh->model_sole_y,
                         es.mesh->model_height});
            if (!es.mesh->socket_info.empty()) {
                CSocketInfo info{};
                for (const auto& [name, si] : es.mesh->socket_info) {
                    info.sockets.emplace(name, SocketInfo{si.kind, si.mirror, si.wrap_of,
                                                          si.lateral_offset_m,
                                                          si.bore_diameter_m,
                                                          si.handle_thickness_m});
                }
                world.registry().emplace<CSocketInfo>(e, std::move(info));
            }
            if (es.mesh->hold.has_value()) {
                const HoldSpec& hs = *es.mesh->hold;
                CHoldProfile hp{};
                if (hs.kind == "one_hand") hp.kind = HoldKind::OneHand;
                else if (hs.kind == "two_hand_line") hp.kind = HoldKind::TwoHandLine;
                else if (hs.kind == "two_hand_wrap") hp.kind = HoldKind::TwoHandWrap;
                else if (hs.kind == "forearm_strap") hp.kind = HoldKind::ForearmStrap;
                hp.aim_aligned = hs.aim_aligned;
                hp.two_hand = hs.two_hand == "required"   ? TwoHandMode::Required
                              : hs.two_hand == "optional" ? TwoHandMode::Optional
                                                          : TwoHandMode::None;
                hp.primary_socket = hs.primary_socket;
                hp.support_socket = hs.support_socket;
                hp.forearm_socket = hs.forearm_socket;
                hp.finger_pose = hs.finger_pose == "trigger"        ? HoldFingerPose::Trigger
                                 : hs.finger_pose == "strap"        ? HoldFingerPose::Strap
                                 : hs.finger_pose == "pinch"        ? HoldFingerPose::Pinch
                                 : hs.finger_pose == "open_support" ? HoldFingerPose::OpenSupport
                                                                    : HoldFingerPose::Wrap;
                hp.has_forward = hs.has_forward;
                hp.forward = hs.forward;
                hp.up = hs.up;
                if (hp.kind != HoldKind::None)
                    world.registry().emplace<CHoldProfile>(e, std::move(hp));
            }
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
        if (es.health.has_value()) {
            CHealth h{};
            h.max = es.health->max;
            h.current = es.health->max;
            h.invuln_after_hit_s = es.health->invuln_after_hit_s;
            world.registry().emplace<CHealth>(e, h);
        }
        if (es.faction.has_value()) {
            world.registry().emplace<CFaction>(
                e, CFaction{es.faction->id, es.faction->hostile_to});
        }
        if (!es.tags.empty()) {
            world.registry().emplace<CTag>(e, CTag{es.tags});
        }
        // Without this, builtin.equip finds no candidate slot on any wearer
        // (EquipMechanic::pick_slot_near returns {} when CEquipSlots is
        // absent) and an authored holster silently never accepts anything.
        if (!es.equip_slots.empty()) {
            CEquipSlots slots{};
            slots.slots.reserve(es.equip_slots.size());
            for (const auto& s : es.equip_slots) {
                EquipSlot out{};
                out.name = s.name;
                out.body_slot = s.body_slot;
                out.host_item_slot = s.host_item_slot;
                out.host_socket = s.host_socket;
                out.local_position = s.local_position;
                out.local_rotation = s.local_rotation;
                out.snap_radius_m = s.snap_radius_m;
                out.exclusive = s.exclusive;
                out.accepts_tags = s.accepts_tags;
                slots.slots.push_back(std::move(out));
            }
            world.registry().emplace<CEquipSlots>(e, std::move(slots));
        }
        // AFTER the authored tags, and appending rather than replacing: the
        // authored path emplaces a fresh CTag, so adding the nesting tag
        // first would silently lose it on every entity that has tags of its
        // own -- which is most of them.
        if (nesting != nullptr && !nesting->tag.empty()) {
            world.registry().get_or_emplace<CTag>(e).tags.push_back(
                nesting->tag);
        }
    }

    // Attachments resolve in a second pass: a weapon may be authored before
    // the character whose hand it belongs in, and a forward reference would
    // otherwise silently drop.
    for (const auto& es : doc.entities) {
        if (!es.attached_to.has_value()) continue;
        const auto child = world.find(scoped(es.id));
        // Both sides are scoped: an attachment inside a nested interior names
        // its parent by the id the AUTHOR wrote, which is unprefixed.
        const auto parent = world.find(scoped(es.attached_to->entity_id));
        if (child == entt::null) continue;
        if (parent == entt::null) {
            PTV_WARN("scene: entity '{}' is attached to unknown entity '{}'",
                     es.id, es.attached_to->entity_id);
            continue;
        }
        CAttachment a{};
        a.parent = static_cast<std::uint32_t>(parent);
        a.anchor = es.attached_to->anchor;
        a.local_position = es.attached_to->position;
        a.local_rotation = es.attached_to->rotation;
        a.child_socket = es.attached_to->socket;
        world.registry().emplace<CAttachment>(child, std::move(a));
    }

    // Bind declared state onto the geometry it describes, matching by id.
    // ``pristine`` is seeded from the same values and never written again:
    // everything that survives a transition is the difference between the
    // two, which is what keeps saves small and keeps the document
    // authoritative for everything nobody touched.
    for (const auto& is : doc.interactives) {
        const auto e = world.find(scoped(is.id));
        if (e == entt::null) {
            PTV_WARN(
                "scene: interactive '{}' has no entity with that id; its "
                "state will not be tracked",
                is.id);
            continue;
        }
        // A "movable" affordance is not a mechanic at all, just a statement
        // that this object's transform should persist. Capture the baseline
        // now, while it is still exactly where the document put it.
        if (is.kind == "movable" && world.registry().all_of<CTransform>(e)) {
            world.registry().emplace_or_replace<CMovable>(
                e, CMovable{world.registry().get<CTransform>(e).t});
        }

        CInteractive ci{};
        // The scoped id, because this IS the persistence key: two nested
        // interiors that both declare a "front_door" must save separately.
        ci.id = scoped(is.id);
        ci.kind = is.kind;
        ci.state = is.initial_state;
        ci.pristine = is.initial_state;
        ci.locked = is.locked;
        ci.hinge_point = is.hinge_point;
        ci.hinge_axis = is.hinge_axis;
        ci.min_angle_deg = is.min_angle_deg;
        ci.max_angle_deg = is.max_angle_deg;
        ci.slide_axis = is.slide_axis;
        ci.slide_distance_m = is.slide_distance_m;
        ci.positions = is.positions;
        ci.yields_asset_id = is.yields_asset_id;
        world.registry().emplace_or_replace<CInteractive>(e, std::move(ci));

        // Declared tags join the entity's own, so an affordance the world
        // author put on the interactive record still reaches the mechanics
        // that search by tag.
        if (!is.tags.empty()) {
            auto& tag = world.registry().get_or_emplace<CTag>(e);
            for (const auto& t : is.tags) {
                if (std::find(tag.tags.begin(), tag.tags.end(), t) ==
                    tag.tags.end()) {
                    tag.tags.push_back(t);
                }
            }
        }
    }
}

}  // namespace ptv
