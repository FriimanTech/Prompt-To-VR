#include "ptv/scene/SceneLoader.h"

#include <fstream>
#include <sstream>

#include "ptv/core/Logger.h"

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
    std::ifstream in(path);
    if (!in) {
        PTV_ERROR("Cannot open scene file: {}", path);
        return {};
    }
    std::stringstream ss;
    ss << in.rdbuf();
    Json j;
    try {
        j = Json::parse(ss.str());
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

    for (const auto& l : j.value("lights", Json::array())) {
        LightSpec ls{};
        ls.id = l.value("id", "");
        ls.kind = l.value("kind", "directional");
        ls.transform = read_transform(l.value("transform", Json::object()));
        ls.color = read_vec3(l.value("color_rgb", Json::array()), Vec3(1.0f));
        ls.intensity = l.value("intensity", 1.0f);
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
            es.mesh = std::move(m);
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
                         es.mesh->target_scale_meters});
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
