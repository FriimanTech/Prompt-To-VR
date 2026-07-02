#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <cmath>

#include "traversal_constructs_shared.h"

namespace ptv {

static entt::entity spawn_ram(World& w, const Vec3& pos, const Quat& rot) {
    auto& reg = w.registry();
    entt::entity e = reg.create();
    // Transform
    reg.emplace<CTransform>(e);
    auto& tr = reg.get<CTransform>(e).t;
    tr.position = pos;
    tr.rotation = rot;
    tr.scale = 1.0f;
    // Mesh
    CMesh mesh{}; mesh.asset_id = "construct_ram_surf"; mesh.glb_path = "assets/construct_ram_surf.glb"; mesh.target_scale_meters = 1.0f;
    reg.emplace<CMesh>(e, std::move(mesh));
    // Physics (kinematic-like; we will position manually)
    CPhysics phys{}; phys.kind = "dynamic"; phys.mass_kg = 40.0f; phys.restitution = 0.0f; phys.friction = 0.9f; phys.collision_shape = "box";
    reg.emplace<CPhysics>(e, std::move(phys));
    // Tags
    CTag tag{}; tag.tags = {"traversal","construct","rideable","ram"};
    reg.emplace<CTag>(e, std::move(tag));
    return e;
}

void ram_update(World& w, std::uint32_t hand_entity, TraversalState& st, const ptv::Vec3& start_pos, const ptv::Quat& hand_rot, float dt, bool cancel) {
    auto& reg = w.registry();

    if (!st.ram.riding) {
        // Spawn slightly ahead of hand
        Vec3 forward = quat_rotate(hand_rot, Vec3{0,0,1});
        Vec3 pos{ start_pos.x + forward.x * 0.6f, start_pos.y + forward.y * 0.2f, start_pos.z + forward.z * 0.6f };
        Quat rot = look_rotation_from_forward_up(forward, Vec3{0,1,0});
        st.ram.ram = spawn_ram(w, pos, rot);
        st.ram.riding = true;
        st.ram.life_s = 30.0f;
        // Haptics: form heavy
        if (w.valid(hand_entity)) publish_haptics(w, static_cast<entt::entity>(hand_entity), "construct_form_heavy", "right", 0.9f, 0.12f);
    }

    if (cancel) {
        if (st.ram.ram != entt::null && reg.valid(st.ram.ram)) reg.destroy(st.ram.ram);
        st.ram = RamRideState{}; // reset
        return;
    }

    // Advance ram forward with stabilization (no roll)
    if (st.ram.ram != entt::null && reg.valid(st.ram.ram)) {
        auto& tr = reg.get<CTransform>(st.ram.ram).t;
        Vec3 forward = quat_rotate(tr.rotation, Vec3{0,0,1});
        tr.position = Vec3{ tr.position.x + forward.x * st.ram.speed_mps * dt,
                            tr.position.y + (st.ram.stabilize ? 0.0f : forward.y * st.ram.speed_mps * dt),
                            tr.position.z + forward.z * st.ram.speed_mps * dt };
        if (st.ram.stabilize) {
            // Keep horizon level: remove roll by recomputing from forward and world up.
            tr.rotation = look_rotation_from_forward_up(forward, Vec3{0,1,0});
        }
        st.ram.life_s -= dt;
        if (st.ram.life_s <= 0.0f) {
            reg.destroy(st.ram.ram);
            st.ram = RamRideState{};
        }
    }
}

} // namespace ptv
