#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// This TU intentionally small: serves as a place where future UI wiring can live if the HUD exposes
// settings mutation callbacks. For now, on_update reads the HUD json through extern decl.

namespace ptv {
// Reserve this symbol so other mechanics/tests can discover the tag keys we publish.
namespace aerial_assist_tags {
    // Keys used inside CTag.tags as key=value strings
    static const char* kEnabled      = "aimassist.enabled";      // 1/0
    static const char* kLeadWorld    = "aimassist.lead_world";   // x,y,z
    static const char* kMagnetDir    = "aimassist.magnet_dir";   // x,y,z (final biased firing dir)
    static const char* kAimDir       = "aimassist.aim_dir";      // x,y,z (raw)
    static const char* kLockRefine   = "aimassist.lock_refine";  // 1/0 (Left Trigger held)
}
}
