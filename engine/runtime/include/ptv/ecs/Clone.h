#pragma once

// Cloning a spawn template into a live actor.
//
// A template is a hidden, fully-formed entity the scene authored: mesh, rig,
// clips, collider, health, faction, mechanic bindings -- and often gear of
// its own (a club in its hand, a lantern, a hat), which is a separate
// template entity attached to it. Spawning has to copy ALL of that. Before
// this lived here, a clone lost its health and faction (an enemy you could
// not shoot and nothing recognised as hostile) and arrived empty-handed.
//
// Pure ECS work so it can be tested without a renderer, physics or
// mechanic host; the caller registers the results with those.

#include <cstdint>
#include <vector>

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

namespace ptv {

struct CloneRecord {
    entt::entity entity{entt::null};
    /** True for gear cloned onto the instance (attached to a cloned parent),
     *  false for the instance itself. */
    bool gear{false};
};

/** Copy everything that describes WHAT ``src`` is onto a new entity at
 *  ``position``/``rotation``: mesh, primitive, physics, bindings, health
 *  (reset to full), faction, equip slots, movable/interactive state, tags
 *  minus ``template``, and the rig with fresh playback state. Transient state
 *  (spring offsets, grab bookkeeping, attachment) is left off. */
entt::entity clone_entity_components(World& world, entt::entity src,
                                     const Vec3& position,
                                     const Quat& rotation);

/** Clone ``src`` and, recursively, every entity attached to it, attaching
 *  each copy to its copied parent. Gear bodies become kinematic -- they are
 *  driven by the parent. ``out`` receives every created entity, the instance
 *  first. Returns the instance, or ``entt::null`` when ``src`` cannot be
 *  cloned. */
entt::entity clone_entity_tree(World& world, entt::entity src,
                               const Vec3& position, const Quat& rotation,
                               std::vector<CloneRecord>& out);

}  // namespace ptv
