#pragma once

// Vignette ("tunnel vision") parameters shared by the continuous-motion
// built-ins: the two slides and smooth turn.
//
// Binding params:
//
//     "vignette": {
//         "enabled": false,        // starting state -- OFF unless asked for
//         "user_toggle": false,    // a settings menu may switch it
//         "strength": 0.7,         // 0..1, how far the edge closes in
//         "inner_fov_deg": 55,     // clear field left at full strength
//         "feather_deg": 25        // width of the soft edge
//     }
//
// A bare boolean ("vignette": true) is accepted as {"enabled": true}.
// Snap turn and teleport deliberately do not read any of this.

#include <algorithm>

#include <nlohmann/json.hpp>

#include "ptv/api/MechanicContext.h"

namespace ptv::comfort {

struct VignetteParams {
    bool enabled{false};
    bool user_toggle{false};
    float strength{0.7f};
    float inner_fov_deg{55.0f};
    float feather_deg{25.0f};
};

inline VignetteParams parse_vignette(const nlohmann::json& params) {
    VignetteParams v;
    if (!params.is_object() || !params.contains("vignette")) return v;
    const auto& j = params["vignette"];
    if (j.is_boolean()) {
        v.enabled = j.get<bool>();
        return v;
    }
    if (!j.is_object()) return v;
    v.enabled = j.value("enabled", false);
    v.user_toggle = j.value("user_toggle", false);
    v.strength = std::clamp(j.value("strength", v.strength), 0.0f, 1.0f);
    v.inner_fov_deg =
        std::clamp(j.value("inner_fov_deg", v.inner_fov_deg), 20.0f, 100.0f);
    v.feather_deg =
        std::clamp(j.value("feather_deg", v.feather_deg), 2.0f, 60.0f);
    return v;
}

/** Offer the vignette for ``channel`` if the binding mentions one at all. */
inline void offer(IComfort* comfort, ComfortChannel channel,
                  const VignetteParams& v) {
    if (comfort == nullptr) return;
    if (v.enabled || v.user_toggle) {
        comfort->offer_vignette(channel, v.enabled, v.user_toggle);
    }
}

}  // namespace ptv::comfort
