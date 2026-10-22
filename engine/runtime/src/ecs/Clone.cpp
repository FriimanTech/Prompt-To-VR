#include "ptv/ecs/Clone.h"

#include <algorithm>

namespace ptv {

entt::entity clone_entity_components(World& world, entt::entity src,
                                     const Vec3& position,
                                     const Quat& rotation) {
    auto& reg = world.registry();
    if (!reg.valid(src) || !reg.all_of<CTransform>(src)) return entt::null;

    const auto dst = world.create_entity();

    CTransform ct = reg.get<CTransform>(src);
    ct.t.position = position;
    ct.t.rotation = rotation;
    reg.emplace<CTransform>(dst, ct);

    // What the thing IS. Transient state (CTransformOffset's spring, grab
    // bookkeeping) is deliberately left off so the clone starts clean.
    if (const auto* m = reg.try_get<CMesh>(src)) reg.emplace<CMesh>(dst, *m);
    if (const auto* p = reg.try_get<CPrimitive>(src)) reg.emplace<CPrimitive>(dst, *p);
    if (const auto* im = reg.try_get<CInlineMesh>(src)) {
        reg.emplace<CInlineMesh>(dst, *im);
    }
    if (const auto* ph = reg.try_get<CPhysics>(src)) reg.emplace<CPhysics>(dst, *ph);
    if (const auto* mb = reg.try_get<CMechanicBinding>(src)) {
        reg.emplace<CMechanicBinding>(dst, *mb);
    }

    // Gameplay identity. Without health a spawned enemy cannot be shot, and
    // without a faction nothing recognises it as an enemy.
    if (const auto* hp = reg.try_get<CHealth>(src)) {
        CHealth h = *hp;
        h.current = h.max;
        h.alive = true;
        h.invuln_s = 0.0f;
        h.death_timer_s = -1.0f;
        reg.emplace<CHealth>(dst, h);
    }
    if (const auto* fa = reg.try_get<CFaction>(src)) reg.emplace<CFaction>(dst, *fa);
    if (const auto* es = reg.try_get<CEquipSlots>(src)) reg.emplace<CEquipSlots>(dst, *es);
    if (const auto* mv = reg.try_get<CMovable>(src)) reg.emplace<CMovable>(dst, *mv);
    if (const auto* ia = reg.try_get<CInteractive>(src)) {
        reg.emplace<CInteractive>(dst, *ia);
    }

    // Tags minus "template": the clone is a real actor, and leaving the
    // marker on would hide it from the renderer and make it spawnable from
    // itself.
    if (const auto* tg = reg.try_get<CTag>(src)) {
        CTag tags{};
        for (const auto& t : tg->tags) {
            if (t == "template") continue;
            tags.tags.push_back(t);
        }
        reg.emplace<CTag>(dst, std::move(tags));
    }

    // The rig is copied wholesale rather than reloaded: the GLB is already
    // parsed and resident, keyed by path. Playback restarts so the clone
    // does not inherit the template's clip time.
    if (const auto* sk = reg.try_get<CSkeleton>(src)) {
        reg.emplace<CSkeleton>(dst, *sk);
        if (const auto* an = reg.try_get<CAnimator>(src)) {
            CAnimator anim = *an;
            anim.time_s = 0.0f;
            anim.blend_t = 0.0f;
            anim.prev_clip = -1;
            std::fill(anim.joint_overridden.begin(), anim.joint_overridden.end(),
                      std::uint8_t{0});
            reg.emplace<CAnimator>(dst, std::move(anim));
        }
    }
    return dst;
}

namespace {

void clone_gear(World& world, entt::entity src_parent, entt::entity dst_parent,
                const Vec3& position, const Quat& rotation,
                std::vector<CloneRecord>& out, int depth) {
    if (depth > 8) return;
    auto& reg = world.registry();
    std::vector<entt::entity> gear;
    {
        auto view = reg.view<CAttachment>();
        const auto parent_id = static_cast<std::uint32_t>(src_parent);
        for (const auto child : view) {
            if (child == src_parent) continue;
            if (view.get<CAttachment>(child).parent == parent_id) {
                gear.push_back(child);
            }
        }
    }
    for (const auto child : gear) {
        const CAttachment att = reg.get<CAttachment>(child);
        const auto copy = clone_entity_components(world, child, position, rotation);
        if (copy == entt::null) continue;
        // Driven by its parent from now on, never simulated.
        if (auto* ph = reg.try_get<CPhysics>(copy)) ph->kind = "kinematic";
        CAttachment a = att;
        a.parent = static_cast<std::uint32_t>(dst_parent);
        reg.emplace_or_replace<CAttachment>(copy, a);
        out.push_back({copy, true});
        clone_gear(world, child, copy, position, rotation, out, depth + 1);
    }
}

}  // namespace

entt::entity clone_entity_tree(World& world, entt::entity src,
                               const Vec3& position, const Quat& rotation,
                               std::vector<CloneRecord>& out) {
    const auto dst = clone_entity_components(world, src, position, rotation);
    if (dst == entt::null) return entt::null;
    out.push_back({dst, false});
    clone_gear(world, src, dst, position, rotation, out, 0);
    return dst;
}

}  // namespace ptv
