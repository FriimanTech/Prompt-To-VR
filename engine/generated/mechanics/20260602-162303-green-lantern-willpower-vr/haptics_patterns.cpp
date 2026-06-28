#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <algorithm>

// This file provides a convenience publish alias and a tiny helper to read pattern presence via params, so
// other generated mechanics can call without depending on the manager class type.

namespace ptv {
namespace haptics {

// Decls from service
enum class Pattern : std::uint8_t;
bool has_pattern(const std::string& id);
void publish(const std::string& pattern_id, const std::string& hand, float intensity_scale, std::optional<float> duration_s);

// Lightweight wrapper: accepts params JSON like { "pattern_id": "bolt_fire_sharp", "hand": "right", "intensity_scale": 1.0, "duration_s": 0.08 }
inline void publish_from_params(const nlohmann::json& j) {
    if (!j.is_object()) return;
    std::string id = j.value("pattern_id", std::string());
    if (id.empty()) return;
    std::string hand = j.value("hand", std::string("both"));
    float scale = j.value("intensity_scale", 1.0f);
    std::optional<float> dur;
    if (j.contains("duration_s")) dur = j.at("duration_s").get<float>();
    publish(id, hand, scale, dur);
}

// Utility: common names array (can be used by tests or debuggers)
static inline const std::array<const char*, 9>& all_patterns() {
    static const std::array<const char*, 9> k = {
        "hover_light",
        "bolt_fire_sharp",
        "construct_form_heavy",
        "recharge_rhythm",
        "block_impact_strong",
        "sprint_start",
        "sprint_end",
        "flight_boost",
        "evade_burst"
    };
    return k;
}

} // namespace haptics
} // namespace ptv
