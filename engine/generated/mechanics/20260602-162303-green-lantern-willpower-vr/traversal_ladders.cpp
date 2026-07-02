#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

#include "traversal_constructs_shared.h"

namespace ptv {

static entt::entity spawn_step(World& w, const Vec3& pos, const Quat& rot, float scale_m) {
    auto& reg = w.registry();
    entt::entity e = reg.create();
    // Transform
    reg.emplace<CTransform>(e);
    auto& tr = reg.get<CTransform>(e).t;
    tr.position = pos;
    tr.rotation = rot;
    tr.scale = 1.0f; // mesh uniform; size via CMesh.target_scale_meters
    // Mesh
    CMesh mesh{}; mesh.asset_id = "construct_ladder_steps"; mesh.glb_path = "assets/construct_ladder_steps.glb"; mesh.target_scale_meters = scale_m;
    reg.emplace<CMesh>(e, std::move(mesh));
    // Physics
    CPhysics phys{}; phys.kind = "static"; phys.mass_kg = 0.0f; phys.restitution = 0.1f; phys.friction = 0.8f; phys.collision_shape = "box";
    reg.emplace<CPhysics>(e, std::move(phys));
    // Tags
    CTag tag{}; tag.tags = {"traversal","construct","ladder_step"};
    reg.emplace<CTag>(e, std::move(tag));
    return e;
}

void ladders_preview_update(World& w, std::uint32_t hand_entity, TraversalState& st, const ptv::Vec3& base_point, const ptv::Vec3& surface_normal, const ptv::Quat& orient, float dt) {
    (void)dt;
    // If count or spacing changed, rebuild preview.
    const float spacing = std::max(0.1f, st.ladder.spacing_m);
    const int count = std::max(1, st.ladder.count);
    const float maxlen = std::max(0.5f, st.ladder.max_length_m);

    // Clamp by max length
    int usable = std::min(count, static_cast<int>(std::floor(maxlen / spacing)));

    // Rebuild if size mismatch
    if ((int)st.ladder.steps.size() != usable) {
        cleanup_spawned(w, st.ladder.steps);
        st.ladder.steps.resize(usable);
        for (int i=0;i<usable;++i) st.ladder.steps[i] = Spawned{};
    }

    // Up vector for step faces: align with world up for comfort; offset along surface normal to float off wall slightly.
    const Vec3 up{0,1,0};
    const Quat step_rot = look_rotation_from_forward_up(Vec3{surface_normal.x, surface_normal.y, surface_normal.z}, up);

    for (int i=0;i<usable;++i) {
        const float d = (i+0.5f) * spacing; // half-step off wall then outwards
        Vec3 pos{ base_point.x + surface_normal.x * d,
                  base_point.y + surface_normal.y * d + i * (spacing * 0.6f), // slight rise per step for climbability
                  base_point.z + surface_normal.z * d };
        if (st.ladder.steps[i].e == entt::null || !w.registry().valid(st.ladder.steps[i].e)) {
            st.ladder.steps[i].e = spawn_step(w, pos, step_rot, 0.3f);
            st.ladder.steps[i].ttl_s = 30.0f; // if confirmed, we keep; else auto-clean when cancelled by preview rebuild
        } else {
            // Update transform live
            auto& tr = w.registry().get<CTransform>(st.ladder.steps[i].e).t;
            tr.position = pos;
            tr.rotation = step_rot;
        }
    }

    // Subtle haptics tick while previewing
    if (w.valid(hand_entity)) {
        publish_haptics(w, static_cast<entt::entity>(hand_entity), "hover_light", "right", 0.2f, 0.03f);
    }
}

void ladders_confirm_finalize(World& w, TraversalState& st) {
    // Make sure existing preview entities persist for 30s; we already set physics static.
    for (auto& s : st.ladder.steps) {
        if (s.e != entt::null && w.registry().valid(s.e)) {
            s.ttl_s = 30.0f;
            // Tag as placed
            if (w.registry().all_of<CTag>(s.e)) {
                auto& tag = w.registry().get<CTag>(s.e);
                tag.tags.push_back("placed");
            }
        }
    }
    st.ladder.confirmed = true;
}

} // namespace ptv
