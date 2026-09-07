#pragma once

#include <string>

#include "ptv/ecs/World.h"
#include "ptv/scene/Scene.h"

namespace ptv {

/** Load the engine-facing scene JSON produced by the scene compiler. */
SceneDoc LoadSceneJson(const std::string& path);

/** How to fold a scene into a world that already has one in it.
 *
 * A nested scene is a second document living INSIDE the current one: an
 * interior authored at human scale and shrunk to fit inside a character's
 * head, a dollhouse, a snow globe. It is not a transition -- the outer world
 * stays loaded and keeps running, because the whole point is that the host
 * walks around while the interior travels with it.
 *
 * Two things have to change on the way in, and both are why this is an option
 * struct rather than a second loader: entity ids have to stop colliding with
 * the outer scene's, and every transform has to be re-expressed relative to
 * wherever the interior is mounted. Doing that here keeps ONE loader; a
 * parallel nested implementation would drift from this one the first time a
 * component was added to a scene document.
 */
struct SceneNesting {
    /** Prepended to every entity id, with a "/" separator.
     *
     *  Required in practice. Two rooms authored by two different world
     *  architects will both contain a "front_door", and ids are persistence
     *  keys -- a collision does not fail loudly, it silently makes one door's
     *  saved state restore onto the other. */
    std::string id_prefix;

    /** Applied to every entity's transform on load.
     *
     *  Mount point and fit scale, composed. The uniform scale is baked into
     *  each entity's own ``Transform::scale`` rather than left in the matrix,
     *  because the attachment system that carries the interior each frame
     *  rewrites position and rotation but deliberately never touches scale. */
    Mat4 mount{1.0f};

    /** Uniform scale factor inside ``mount``, needed separately for the
     *  reason above. */
    float scale{1.0f};

    /** Added to every entity, so the whole interior can be found, hidden or
     *  unloaded as a unit. */
    std::string tag;
};

/** Populate ``world`` from ``doc``. GLB files referenced in doc.entities are
 *  loaded on demand by the renderer; this just creates entities + components.
 *
 *  ``nesting`` is null for a normal top-level load. */
void ApplySceneToWorld(const SceneDoc& doc, World& world,
                       const SceneNesting* nesting = nullptr);

}  // namespace ptv
