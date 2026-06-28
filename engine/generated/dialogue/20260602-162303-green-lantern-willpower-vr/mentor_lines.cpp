#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace mentor_lines {

// Non-copyrighted, paraphrased guidance lines.
// Grouped by lightweight context keys consumed by the hook.

static const std::vector<std::string> kOnboard_CentralPlaza = {
    "Welcome, recruit. This plaza is your staging ground.",
    "Look toward the Central Battery. Breathe, steady your hands.",
    "Your ring shapes thought into action. Keep your focus light and direct."
};

static const std::vector<std::string> kOnboard_TrainingArena = {
    "Training ground ahead. Start simple: form a shield, then a clean strike.",
    "Mind your stance. Small movements read sharply in the ring.",
    "Build rhythm: guard, move, then commit."
};

static const std::vector<std::string> kTip_RepeatedFailure = {
    "Easy does it. Read the field before you rush.",
    "Lead with cover, then counter. Your will guides order into the chaos.",
    "If you’re pressed, shape a barrier and reset your angle."
};

static const std::vector<std::string> kPrompt_Recharge = {
    "Power dipping. Approach the recharge pedestal and align the ring.",
    "Hold still a moment. Let the current settle and restore your charge.",
    "When topped up, your constructs land clearer and hit truer."
};

static const std::vector<std::string> kOnboard_Generic = {
    "Eyes up. Center your breath, set your intent.",
    "Your will is the tool; the ring is the lens.",
    "Keep motions smooth—clean intent shapes clean light."
};

inline const std::vector<std::string>& select_block(const std::string& context,
                                                    const std::string& scene_id,
                                                    const std::string& reason)
{
    if (context == "scene_entry") {
        if (scene_id == "central_battery_plaza") return kOnboard_CentralPlaza;
        if (scene_id == "training_arena") return kOnboard_TrainingArena;
        return kOnboard_Generic;
    }
    if (context == "repeated_failure" || reason == "repeated_failure") {
        return kTip_RepeatedFailure;
    }
    if (context == "recharge_prompt" || reason == "low_energy_near_station") {
        return kPrompt_Recharge;
    }
    return kOnboard_Generic;
}

} // namespace mentor_lines
