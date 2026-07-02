#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>

namespace ptv {

// Lightweight mission/objective schema local to this task
struct ObjectiveDef {
    std::string id;            // e.g., "defend_reactors"
    std::string type;          // e.g., "defend|escort|cleanse|disable|rescue|defeat"
    int target_count{1};       // count-based objectives (kills, rescues, pylons, etc.)
};

struct ChallengeDef {
    std::string id;            // optional challenge id
    std::string rule;          // free-form rule id (e.g., "no_damage", "time_under")
    float threshold{0.0f};
};

struct DifficultyTier {
    std::string id;            // "easy|normal|hard|legendary" (free-form)
    float ai_scale{1.0f};      // used by AI packs to scale aggression/spawns via tags
    float spawn_multiplier{1.0f};
    float score_multiplier{1.0f};
};

struct MissionDef {
    std::string id;
    std::vector<std::string> scenes;                 // ordered scene ids for travel
    std::vector<ObjectiveDef> objectives;            // linear for MVP; branches in future
    std::vector<ChallengeDef> challenges;            // optional
    std::vector<DifficultyTier> tiers;               // difficulty options
    int base_reward_xp{0};                           // baseline XP before multipliers
};

struct MedalThresholds {
    // per-mission thresholds (seconds for time trials, or score numbers)
    // map: mission_id -> { bronze, silver, gold }
    struct Trio { float bronze{0}, silver{0}, gold{0}; };
    std::unordered_map<std::string, Trio> by_mission;
};

struct ObjectiveProgress {
    int current{0};
    int target{1};
    bool completed{false};
};

struct MissionRuntimeState {
    bool has_selection{false};
    std::string selected_mission_id;
    std::string selected_difficulty{"normal"};
    std::size_t objective_index{0};
    std::unordered_map<std::string, ObjectiveProgress> objective_progress; // by objective id
    float elapsed_s{0.0f};
    bool in_progress{false};
    bool finished{false};
    bool failed{false};
    int score{0};
    std::optional<std::string> medal; // "bronze|silver|gold"
};

// Local helper utilities (declarations)
namespace mission_util {

inline void push_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.valid(e)) return;
    if (!w.has<CTag>(e)) {
        w.emplace_or_replace<CTag>(e, CTag{});
    }
    auto& tags = w.get<CTag>(e).tags;
    tags.emplace_back(tag);
}

inline std::string make_tag(const std::string& prefix, const nlohmann::json& payload) {
    return prefix + payload.dump();
}

// Parse helpers
void parse_missions(const nlohmann::json& j, std::unordered_map<std::string, MissionDef>& out);
MedalThresholds parse_medal_thresholds(const nlohmann::json& j);

// Runtime helpers
void reset_runtime_for_mission(const MissionDef& def, MissionRuntimeState& rt);
void tick_objectives_from_tags(World& w, MissionRuntimeState& rt, const MissionDef& def, const std::string& objectiveProgressPrefix, std::uint32_t selfEntity);
void maybe_award_medal_and_persist(World& w, std::uint32_t e, const MissionDef& def, MissionRuntimeState& rt, const MedalThresholds& thresholds,
                                   const std::string& saveSectionPushPrefix, const std::string& saveRequestPrefix,
                                   const std::string& uiToastPrefix, const std::string& missionCompletePrefix);

// UI helpers
void ensure_wrist_page_registered(World& w, std::uint32_t e, const std::string& uiPageRegisterPrefix);
void update_wrist_status(World& w, std::uint32_t e, const MissionDef* def, const MissionRuntimeState& rt, const std::string& uiToastPrefix, float toastCooldownReset);

} // namespace mission_util

} // namespace ptv
