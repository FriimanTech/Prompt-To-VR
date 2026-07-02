#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "locomotion_options_ui_shared.h"

// This file intentionally left minimal; the first-time toast is posted from on_attach
// in the register.cpp to ensure it fires as soon as the rig spawns. This TU exists to
// keep the suggested split and leave room for future scene-conditional hints.
