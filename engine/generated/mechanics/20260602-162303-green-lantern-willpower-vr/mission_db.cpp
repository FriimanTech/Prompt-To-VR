#include "mission_shared.h"

#include <cstddef>

namespace ptv { namespace mission_util {

static ObjectiveDef parse_objective(const nlohmann::json& j) {
    ObjectiveDef o;
    if (j.is_object()) {
        o.id = j.value("id", std::string{});
        o.type = j.value("type", std::string{"generic"});
        o.target_count = j.value("target_count", 1);
    }
    return o;
}

static DifficultyTier parse_tier(const nlohmann::json& j) {
    DifficultyTier t;
    if (j.is_object()) {
        t.id = j.value("id", std::string{"normal"});
        t.ai_scale = j.value("ai_scale", 1.0f);
        t.spawn_multiplier = j.value("spawn_multiplier", 1.0f);
        t.score_multiplier = j.value("score_multiplier", 1.0f);
    }
    return t;
}

void parse_missions(const nlohmann::json& j, std::unordered_map<std::string, MissionDef>& out) {
    out.clear();
    if (!j.is_array()) return;
    for (const auto& m : j) {
        if (!m.is_object()) continue;
        MissionDef def;
        def.id = m.value("id", std::string{});
        def.base_reward_xp = m.value("base_reward_xp", 0);
        if (m.contains("scenes") && m["scenes"].is_array()) {
            for (const auto& s : m["scenes"]) def.scenes.emplace_back(s.get<std::string>());
        }
        if (m.contains("objectives") && m["objectives"].is_array()) {
            for (const auto& o : m["objectives"]) def.objectives.emplace_back(parse_objective(o));
        }
        if (m.contains("challenges") && m["challenges"].is_array()) {
            for (const auto& c : m["challenges"]) {
                if (!c.is_object()) continue;
                ChallengeDef ch; ch.id = c.value("id", std::string{}); ch.rule = c.value("rule", std::string{}); ch.threshold = c.value("threshold", 0.0f);
                def.challenges.emplace_back(std::move(ch));
            }
        }
        if (m.contains("difficulty_tiers") && m["difficulty_tiers"].is_array()) {
            for (const auto& t : m["difficulty_tiers"]) def.tiers.emplace_back(parse_tier(t));
        }
        if (!def.id.empty()) out[def.id] = std::move(def);
    }
}

MedalThresholds parse_medal_thresholds(const nlohmann::json& j) {
    MedalThresholds mt;
    if (!j.is_object()) return mt;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string mid = it.key();
        MedalThresholds::Trio trio{};
        if (it.value().is_object()) {
            trio.bronze = it.value().value("bronze", 0.0f);
            trio.silver = it.value().value("silver", 0.0f);
            trio.gold   = it.value().value("gold",   0.0f);
        }
        mt.by_mission[mid] = trio;
    }
    return mt;
}

void reset_runtime_for_mission(const MissionDef& def, MissionRuntimeState& rt) {
    rt.objective_index = 0;
    rt.in_progress = false;
    rt.finished = false;
    rt.failed = false;
    rt.elapsed_s = 0.0f;
    rt.score = 0;
    rt.medal.reset();
    rt.objective_progress.clear();
    for (const auto& o : def.objectives) {
        rt.objective_progress[o.id] = ObjectiveProgress{0, std::max(1, o.target_count), false};
    }
}

}} // namespace ptv::mission_util
