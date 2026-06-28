#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace ptv {
struct MissionMedalThresholds {
    // Time in seconds for time-trial style medals, or score thresholds otherwise.
    // Lower is better for time, higher is better for score. We treat smaller_is_better flag per-mission.
    float bronze{0.0f};
    float silver{0.0f};
    float gold{0.0f};
    bool smaller_is_better{true};
};

struct ObjectiveDef {
    std::string id;          // unique within mission
    std::string kind;        // defend / escort / cleanse / disable / rescue / defeat_boss / reach_gate / time_trial
    float target_value{1.0f}; // e.g., count to reach, seconds to survive, etc.
    std::string description; // short description for HUD
};

struct MissionDef {
    std::string id;                    // e.g., zero_g_flight_gauntlet
    std::vector<std::string> scenes;   // scene ids in order
    std::vector<ObjectiveDef> chain;   // ordered list; simple linear for now
    std::optional<MissionMedalThresholds> medals; // optional medal rules
    std::string difficulty_profile;    // name of profile family to scale others with
};

struct MissionRuntimeState {
    bool mission_active{false};
    std::string mission_id;
    int current_obj_index{-1};
    float current_obj_progress{0.0f};
    float score{0.0f};
    float elapsed_s{0.0f};
    std::string selected_difficulty{"normal"};
    std::string last_awarded_medal; // bronze / silver / gold
};

// Lightweight DB holding all configured missions.
struct MissionDB {
    std::unordered_map<std::string, MissionDef> by_id;
};

namespace mission_api {
// Public surface for other mechanics.
void post_objective_progress(std::uint32_t actor_entity, float amount); // advances current objective by amount
void set_score_delta(float amount);
std::string current_mission_id();
std::string current_objective_id();
bool mission_in_progress();
std::string current_difficulty();
float mission_elapsed();
}

} // namespace ptv
