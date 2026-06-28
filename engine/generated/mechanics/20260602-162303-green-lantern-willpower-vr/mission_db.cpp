#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

namespace ptv {

struct ObjectiveDef {
    std::string id;
    std::string kind;
    float target_value{1.0f};
    std::string description;
};

struct MissionMedalThresholds {
    float bronze{0.0f};
    float silver{0.0f};
    float gold{0.0f};
    bool smaller_is_better{false};
};

struct MissionDef {
    std::string id;
    std::vector<std::string> scenes;
    std::vector<ObjectiveDef> chain;
    std::optional<MissionMedalThresholds> medals;
    std::string difficulty_profile;
};

struct MissionDB {
    std::unordered_map<std::string, MissionDef> by_id;
};

namespace {

static MissionDef make_zero_g_gauntlet_default() {
    MissionDef m;
    m.id = "zero_g_flight_gauntlet";
    m.scenes = {"zero_g_flight_gauntlet"};
    m.chain = {
        ObjectiveDef{ "reach_gate", "time_trial", 1.0f, "Sprint through the gauntlet and finish fast." }
    };
    MissionMedalThresholds t;
    // Reasonable placeholder thresholds (seconds): Bronze 90, Silver 70, Gold 55
    t.bronze = 90.0f; t.silver = 70.0f; t.gold = 55.0f; t.smaller_is_better = true;
    m.medals = t;
    m.difficulty_profile = "flight_gauntlet";
    return m;
}

static MissionDef make_civilian_rescue_default() {
    MissionDef m;
    m.id = "civilian_rescue_zone";
    m.scenes = {"civilian_rescue_zone"};
    m.chain = {
        ObjectiveDef{ "rescue_civilians", "rescue", 5.0f, "Rescue 5 civilians." }
    };
    m.medals.reset();
    m.difficulty_profile = "rescue_ops";
    return m;
}

static MissionDef make_alien_city_defense_default() {
    MissionDef m;
    m.id = "alien_city_attack";
    m.scenes = {"alien_city_attack"};
    m.chain = {
        ObjectiveDef{ "defend_city", "defend", 100.0f, "Hold for 100 seconds." }
    };
    m.medals.reset();
    m.difficulty_profile = "defense";
    return m;
}

static void try_parse_medals(const nlohmann::json& j, MissionMedalThresholds& out) {
    if (j.contains("bronze")) out.bronze = j.at("bronze").get<float>();
    if (j.contains("silver")) out.silver = j.at("silver").get<float>();
    if (j.contains("gold")) out.gold = j.at("gold").get<float>();
    if (j.contains("smaller_is_better")) out.smaller_is_better = j.at("smaller_is_better").get<bool>();
}

static void try_parse_objectives(const nlohmann::json& arr, std::vector<ObjectiveDef>& chain) {
    for (const auto& oj : arr) {
        if (!oj.is_object()) continue;
        ObjectiveDef o;
        o.id = oj.value("id", std::string{});
        o.kind = oj.value("kind", std::string{"custom"});
        o.target_value = oj.value("target", 1.0f);
        o.description = oj.value("description", std::string{});
        if (!o.id.empty()) chain.push_back(o);
    }
}

} // namespace

// Build DB either from binding params or fall back to defaults.
static MissionDB build_db_from_params(const nlohmann::json& params) {
    MissionDB db;
    if (params.is_object() && params.contains("missions") && params.at("missions").is_array()) {
        for (const auto& mj : params.at("missions")) {
            if (!mj.is_object()) continue;
            MissionDef m;
            m.id = mj.value("id", std::string{});
            if (m.id.empty()) continue;
            if (mj.contains("scenes") && mj.at("scenes").is_array()) {
                for (const auto& s : mj.at("scenes")) m.scenes.push_back(s.get<std::string>());
            }
            if (mj.contains("objectives") && mj.at("objectives").is_array()) {
                try_parse_objectives(mj.at("objectives"), m.chain);
            }
            if (mj.contains("medal_thresholds") && mj.at("medal_thresholds").is_object()) {
                MissionMedalThresholds t{}; try_parse_medals(mj.at("medal_thresholds"), t); m.medals = t;
            }
            m.difficulty_profile = mj.value("difficulty_profile", std::string{"default"});
            db.by_id.emplace(m.id, std::move(m));
        }
    }

    // Ensure key demo missions exist for test hooks
    if (!db.by_id.count("zero_g_flight_gauntlet")) db.by_id.emplace("zero_g_flight_gauntlet", make_zero_g_gauntlet_default());
    if (!db.by_id.count("civilian_rescue_zone")) db.by_id.emplace("civilian_rescue_zone", make_civilian_rescue_default());
    if (!db.by_id.count("alien_city_attack")) db.by_id.emplace("alien_city_attack", make_alien_city_defense_default());
    return db;
}

// Expose to other translation units in this mechanic.
MissionDB g_mission_db; // defined here, referenced in register/runtime files

void mission_db_ensure_built_from(const nlohmann::json& params) {
    g_mission_db = build_db_from_params(params);
}

} // namespace ptv
