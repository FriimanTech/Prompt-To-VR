#include "mentor_dialogue_shared.h"

#include <string>
#include <vector>

namespace ptv {

static std::vector<MentorLine> onboarding_central_plaza() {
    return {
        {"Mentor", "Welcome, recruit. Keep your stance relaxed and your gaze level. We'll begin with simple movement.", 4.0f, std::nullopt},
        {"Mentor", "Hover is enabled here. A light touch goes farther than force.", 3.5f, std::nullopt},
        {"Mentor", "Your ring shapes intent. Try a basic shield or a bolt to feel the response.", 4.0f, std::nullopt}
    };
}

static std::vector<MentorLine> onboarding_training_arena() {
    return {
        {"Mentor", "This arena is for practice. Watch your spacing and read enemy tells.", 3.5f, std::nullopt},
        {"Mentor", "Form follows focus. Short bursts build confidence—then commit.", 3.5f, std::nullopt},
        {"Mentor", "Defend, reposition, then strike. You'll set the rhythm, not them.", 3.5f, std::nullopt}
    };
}

static std::vector<MentorLine> failure_tip_generic() {
    return {
        {"Mentor", "Breathe. Reset your stance. Use a barrier to buy a moment.", 3.0f, std::nullopt},
        {"Mentor", "Angles matter. Step off the threat line, then answer with purpose.", 3.0f, std::nullopt}
    };
}

static std::vector<MentorLine> failure_tip_hazard() {
    return {
        {"Mentor", "Hazards punish hurry. Mark the pattern, then move between beats.", 3.2f, std::nullopt},
        {"Mentor", "A small platform can bridge danger—shape it steady, then cross.", 3.2f, std::nullopt}
    };
}

static std::vector<MentorLine> recharge_prompt_lines() {
    return {
        {"Mentor", "Your charge is low. Center yourself at the station and let the light flow.", 3.5f, std::nullopt},
        {"Mentor", "Steady breath in, slow breath out. Focus gathers and the ring answers.", 3.5f, std::nullopt}
    };
}

std::vector<MentorLine> build_lines_for_topic(const std::string& topic,
                                              const std::string& scene,
                                              const std::string& failure_kind)
{
    if (topic == "onboarding") {
        if (scene == "central_battery_plaza") return onboarding_central_plaza();
        if (scene == "training_arena") return onboarding_training_arena();
        // Default onboarding
        return {
            {"Mentor", "Welcome. We start simple—move with intent and stay aware.", 3.5f, std::nullopt},
            {"Mentor", "Test a light construct. Feel how control tracks your focus.", 3.5f, std::nullopt}
        };
    }
    if (topic == "failure_tip") {
        if (failure_kind == "hazard") return failure_tip_hazard();
        return failure_tip_generic();
    }
    if (topic == "recharge_prompt") {
        return recharge_prompt_lines();
    }
    // Fallback
    return { {"Mentor", "Steady now. Choose the next clear step and take it.", 3.0f, std::nullopt} };
}

} // namespace ptv
