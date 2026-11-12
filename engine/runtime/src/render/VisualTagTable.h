#pragma once

// ---------------------------------------------------------------------------
// One table for every gameplay tag the renderer reacts to.
//
// These decisions used to be string comparisons scattered through four places
// in the draw loop: a "template" check near the top, an "enemy"/"grabbable"
// tint further down, a "ground" test in the primitive path, and another
// "ground"/"grabbable"/"trigger" test in the placeholder-cube path. Adding one
// affordance meant finding all four and hoping none were missed -- and one WAS
// missed, which is why a declared door drew as an orange grab-cube.
//
// So the mapping lives here, as data, and the renderer resolves an entity's
// tags ONCE per frame into a ``TagVisuals``. Adding an affordance is a row.
//
// What this file deliberately does NOT decide: whether an object is currently
// interactable. That depends on reach, on whether the object is locked, and on
// per-scene interaction policy -- all of which the affordance built-ins
// already evaluate every frame. They publish the answer as ``CHighlight`` and
// the renderer just draws it. A renderer that recomputed it would be a second
// source of truth, and the two would disagree: an object would glow while its
// mechanic refused to act on it, which is worse than no cue at all.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv::render_tags {

/** How a tag affects drawing. A tag may carry more than one role. */
enum class Role : std::uint32_t {
    kNone = 0,
    /** Never draw this entity, mesh or not. Construct prefabs parked at a
     *  staging coordinate for runtime spawning. */
    kAlwaysHidden = 1u << 0,
    /** Draw only when the entity has a real mesh. Marker entities (spawn
     *  anchors, tutorial beacons, checkpoints) carry a transform and nothing
     *  to look at; without this they fell through to the procedural cube path
     *  and littered the world with debug cubes on top of the real
     *  environment. */
    kHiddenWhenMeshless = 1u << 1,
    /** Floor: oversized and given the grid material, so the play-area edge
     *  never shows in the headset. */
    kGround = 1u << 2,
    /** Has a resting tint applied to its mesh (distinct from the hover cue). */
    kMeshTint = 1u << 3,
    /** Has a distinct placeholder appearance when it has no mesh yet. */
    kPlaceholder = 1u << 4,
};

inline Role operator|(Role a, Role b) {
    return static_cast<Role>(static_cast<std::uint32_t>(a) |
                             static_cast<std::uint32_t>(b));
}
inline bool has_role(Role set, Role bit) {
    return (static_cast<std::uint32_t>(set) &
            static_cast<std::uint32_t>(bit)) != 0u;
}

/** The resolved outcome for one entity, after all its tags are considered. */
struct TagVisuals {
    bool always_hidden = false;
    bool hidden_when_meshless = false;
    bool is_ground = false;

    bool has_mesh_tint = false;
    Vec4 mesh_tint{1.0f, 1.0f, 1.0f, 1.0f};

    bool has_placeholder = false;
    float placeholder_xz_scale = 0.3f;
    float placeholder_y_scale = 0.3f;
    Vec4 placeholder_color{0.78f, 0.78f, 0.82f, 1.0f};
    float placeholder_material_id = 0.0f;
};

/** Whether an entity with nothing to draw (no mesh, primitive or inline
 *  mesh) is drawn as a placeholder shape. Only when a tag ASKS for one
 *  (``ground``, ``door``, ``placeholder`` ...): the old default cube drew
 *  every logic host, anchor and collider proxy -- the player's finger
 *  colliders among them, as white boxes over their hands. */
inline bool draws_meshless(const TagVisuals& v) {
    return !v.always_hidden && !v.hidden_when_meshless && v.has_placeholder;
}

/** Resolve every tag on one entity in a single pass.
 *
 *  Earlier rows win for any field already set, so the table's order is its
 *  priority: ``ground`` before ``grabbable`` means a floor tagged both draws
 *  as a floor. */
TagVisuals resolve(const std::vector<std::string>& tags);

/** Every tag in the table. Exposed so a test can assert the renderer's
 *  vocabulary matches the affordance kinds the world architects may emit --
 *  the check that would have caught the missed door. */
const std::vector<std::string>& known_tags();

}  // namespace ptv::render_tags
