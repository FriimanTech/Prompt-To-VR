#pragma once

// Who a shot / strike may hurt, shared by builtin.weapon, builtin.melee_weapon
// and the projectile system so every weapon agrees.
//
//   * the struck collider is resolved to its HEALTH OWNER: a bullet that hits
//     the rifle an NPC holds, or a helmet attached to it, damages the NPC;
//   * hostile factions can always be hurt; things on no side (a target
//     dummy, an exploding barrel) can always be hurt; allies of the shooter
//     cannot (no friendly fire on the player's squad).

#include <cstdint>
#include <cstring>

#include "ptv/api/MechanicContext.h"
#include "ptv/ecs/World.h"

namespace ptv::combat_rules {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

inline std::uint32_t health_owner(const World& world, std::uint32_t entity) {
    std::uint32_t e = entity;
    for (int i = 0; i < 4 && e != kNone && world.valid(e); ++i) {
        if (world.has<CHealth>(e)) return e;
        if (!world.has<CAttachment>(e)) break;
        e = world.get<CAttachment>(e).parent;
    }
    return entity;
}

/** Where to aim at something: the centre of its collider when it has one
 *  (a drone's origin is wherever its GLB put it), else a character's chest.
 *  A character with a model frame has its origin at half its height
 *  (``foot_offset_m``), so "origin plus a metre" aimed over a bruiser's head
 *  into the sky; one without stands on its origin. */
inline Vec3 aim_point(const World& world, std::uint32_t entity,
                      const IPhysicsAccess* physics = nullptr) {
    if (!world.has<CTransform>(entity)) return Vec3(0.0f);
    const Vec3 p = world.get<CTransform>(entity).t.position;
    const float half = world.has<CMesh>(entity) ? world.get<CMesh>(entity).foot_offset_m : 0.0f;
    // A character with a model frame: its origin is its middle, the chest
    // a little above. Walking characters are moved by their transform, and
    // their body can trail it, so the transform is the truth for them.
    if (half > 1e-3f) return p + Vec3(0.0f, half * 0.45f, 0.0f);
    // Anything else (a drone, a turret, a crate): its collider's centre,
    // when the body is where the thing is drawn.
    Vec3 c(0.0f);
    if (physics != nullptr && physics->body_center(entity, &c) && glm::length(c - p) < 3.0f) {
        return c;
    }
    return p + Vec3(0.0f, 1.2f, 0.0f);
}

inline bool may_damage(const ICombat* combat, std::uint32_t shooter, std::uint32_t target) {
    if (combat == nullptr || target == kNone || target == shooter) return false;
    if (combat->max_health(target) <= 0.0f || !combat->is_alive(target)) return false;
    if (shooter == kNone || combat->is_hostile(shooter, target)) return true;
    const char* tf = combat->faction(target);
    if (tf == nullptr || tf[0] == '\0' || std::strcmp(tf, "neutral") == 0) return true;
    const char* sf = combat->faction(shooter);
    if (sf == nullptr || sf[0] == '\0') return true;
    return std::strcmp(sf, tf) != 0 && std::strcmp(tf, "ally") != 0 &&
           std::strcmp(tf, "player") != 0;
}

}  // namespace ptv::combat_rules
