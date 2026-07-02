#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace ptv {

struct MentorLine {
    std::string speaker;
    std::string text;
    float ttl_s{3.0f};
    // Optional lightweight VFX hint (consumed by other mechanics listening to CTag)
    // Example payload emitted as: vfx.spawn:{"asset_id":"central_battery_structure","ttl_s":2.0}
    std::optional<std::string> vfx_asset_id;
};

struct MentorSessionState {
    std::vector<MentorLine> lines;
    std::size_t cursor{0};
    bool mute_vo{false};
};

// Topic keys expected via builtin.dialogue_trigger params_json
// topic: "onboarding" | "failure_tip" | "recharge_prompt"
// scene (optional): "central_battery_plaza" | "training_arena"
// failure_kind (optional): "hazard" | "timed_out" | "low_energy"

} // namespace ptv
