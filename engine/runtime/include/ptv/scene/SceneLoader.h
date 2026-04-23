#pragma once

#include <string>

#include "ptv/ecs/World.h"
#include "ptv/scene/Scene.h"

namespace ptv {

/** Load the engine-facing scene JSON produced by the scene compiler. */
SceneDoc LoadSceneJson(const std::string& path);

/** Populate ``world`` from ``doc``. GLB files referenced in doc.entities are
 *  loaded on demand by the renderer; this just creates entities + components. */
void ApplySceneToWorld(const SceneDoc& doc, World& world);

}  // namespace ptv
