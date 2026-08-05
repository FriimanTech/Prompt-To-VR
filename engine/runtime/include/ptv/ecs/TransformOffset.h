#pragma once

// ---------------------------------------------------------------------------
// ptv::tick_transform_offsets -- per-frame decay for CTransformOffset.
//
// The transform-offset layer is the engine's "juice" channel: a short-lived,
// purely visual displacement (recoil kick, hit flinch, impact shove) that
// springs back to rest. It is deliberately separate from CTransform because
// physics owns CTransform for any entity with a body -- see the comment on
// CTransformOffset in ptv/ecs/World.h.
//
// Call this once per frame AFTER the Simulate-phase mechanics have run (so a
// kick requested this frame is visible this frame) and BEFORE the renderer
// builds its draw list.
// ---------------------------------------------------------------------------

#include "ptv/ecs/World.h"

namespace ptv {

/** Advance every active ``CTransformOffset`` toward its target.
 *
 * Frame-rate independent: the step is ``1 - exp(-dt/tau)``, matching the
 * ``slide::ComfortRamp`` convention used by the locomotion built-ins, so a
 * kick decays over the same wall-clock time at 72 Hz and 90 Hz. Offsets that
 * have settled within epsilon of their target snap exactly onto it and clear
 * ``active`` so subsequent frames skip them entirely. */
void tick_transform_offsets(World& world, float dt);

/** Compose an entity's draw-time model matrix: its ``CTransform`` with any
 * active ``CTransformOffset`` applied in local space.
 *
 * Returns plain ``t.matrix()`` when the entity has no active offset, so the
 * common case costs one component lookup and no extra matrix work. */
Mat4 entity_visual_matrix(const World& world, std::uint32_t entity,
                          const Transform& t);

}  // namespace ptv
