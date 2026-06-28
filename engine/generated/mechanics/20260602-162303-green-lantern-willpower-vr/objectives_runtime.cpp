#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <algorithm>

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

struct MissionRuntimeState {
    std::string mission_id;
    bool mission_active{false};
    int current_obj_index{-1};
    float current_obj_progress{0.0f};
    float score{0.0f};
    float elapsed_s{0.0f};
    std::string last_awarded_medal;
    std::string selected_difficulty{"normal"};
};

extern MissionDB g_mission_db; // from mission_db.cpp

namespace {
struct GlobalRuntime {
    // One global mission runtime per-process for now (single-player)
    MissionRuntimeState rt;
} g_runtime;

static const MissionDef* current_def() {
    if (g_runtime.rt.mission_id.empty()) return nullptr;
    auto it = g_mission_db.by_id.find(g_runtime.rt.mission_id);
    if (it == g_mission_db.by_id.end()) return nullptr;
    return &it->second;
}

static void ensure_selected_or_default() {
    if (!g_runtime.rt.mission_id.empty()) return;
    // Prefer gauntlet for acceptance
    if (g_mission_db.by_id.count("zero_g_flight_gauntlet")) g_runtime.rt.mission_id = "zero_g_flight_gauntlet";
    else if (!g_mission_db.by_id.empty()) g_runtime.rt.mission_id = g_mission_db.by_id.begin()->first;
}

static void tag_portal_ready(World& w, const std::string& scene_id) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        auto& tags = reg.get<CTag>(e).tags;
        bool is_portal = std::any_of(tags.begin(), tags.end(), [](const std::string& t){return t=="portal_area" || t=="portal_stub" || t=="nav_console";});
        if (!is_portal) continue;
        // Advertise next scene via tags for the portal_travel system to consume later.
        // key: portal:target_scene:<scene_id>
        const std::string key = "portal:target_scene";
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){return s.rfind(key+":",0)==0;}), tags.end());
        tags.push_back(key + ":" + scene_id);
        const std::string arm = "portal:armed";
        tags.erase(std::remove(tags.begin(), tags.end(), arm), tags.end());
        tags.push_back(arm);
    }
}

} // namespace

// Public API impl
namespace mission_api {

void post_objective_progress(std::uint32_t /*actor_entity*/, float amount) {
    auto* def = current_def(); if (!def) return;
    if (!g_runtime.rt.mission_active || g_runtime.rt.current_obj_index < 0) return;
    g_runtime.rt.current_obj_progress += amount;
    const auto& obj = def->chain[g_runtime.rt.current_obj_index];
    if (g_runtime.rt.current_obj_progress >= obj.target_value) {
        // Advance to next objective or finish
        g_runtime.rt.current_obj_index++;
        g_runtime.rt.current_obj_progress = 0.0f;
        if (g_runtime.rt.current_obj_index >= (int)def->chain.size()) {
            // Completed mission
            g_runtime.rt.mission_active = false;
        }
    }
}

void set_score_delta(float amount) { g_runtime.rt.score += amount; }
std::string current_mission_id() { return g_runtime.rt.mission_id; }
std::string current_objective_id() {
    auto* def = current_def();
    if (!def) return {};
    if (g_runtime.rt.current_obj_index < 0 || g_runtime.rt.current_obj_index >= (int)def->chain.size()) return {};
    return def->chain[g_runtime.rt.current_obj_index].id;
}
bool mission_in_progress() { return g_runtime.rt.mission_active; }
std::string current_difficulty() { return g_runtime.rt.selected_difficulty; }
float mission_elapsed() { return g_runtime.rt.elapsed_s; }

} // namespace mission_api

// Runtime entry points used by MissionSystem
void mission_runtime_begin_selected(World& w) {
    ensure_selected_or_default();
    auto* def = current_def(); if (!def) return;
    g_runtime.rt.mission_active = true;
    g_runtime.rt.current_obj_index = def->chain.empty()? -1 : 0;
    g_runtime.rt.current_obj_progress = 0.0f;
    g_runtime.rt.score = 0.0f;
    g_runtime.rt.elapsed_s = 0.0f;
    // Arm portal to first scene listed
    if (!def->scenes.empty()) tag_portal_ready(w, def->scenes.front());
}

void mission_runtime_cancel() {
    g_runtime.rt = MissionRuntimeState{}; // reset to defaults
}

void mission_runtime_tick(World& w, float dt) {
    auto* def = current_def();
    if (!def) return;
    if (g_runtime.rt.mission_active) {
        g_runtime.rt.elapsed_s += dt;
        // Simple auto-progression for time_trial reach_gate: treat objective as done when a tag appears (e.g., gate touched).
        if (g_runtime.rt.current_obj_index >= 0 && g_runtime.rt.current_obj_index < (int)def->chain.size()) {
            const auto& obj = def->chain[g_runtime.rt.current_obj_index];
            if (obj.kind == "time_trial") {
                // If a finish beacon entity tagged "finish_beacon" exists and also has a tag "beacon:touched",
                // consider the objective complete. Tests can flip that tag via scripted harness.
                auto& reg = w.registry();
                auto view = reg.view<CTag>();
                bool finished = false;
                for (auto e : view) {
                    const auto& tags = reg.get<CTag>(e).tags;
                    bool is_finish = std::find(tags.begin(), tags.end(), "finish_beacon") != tags.end();
                    bool touched = std::find(tags.begin(), tags.end(), "beacon:touched") != tags.end();
                    if (is_finish && touched) { finished = true; break; }
                }
                if (finished) {
                    mission_api::post_objective_progress(0, obj.target_value);
                }
            }
        }
    }

    // Drive UI tags
    extern void mission_ui_update(World&, const MissionRuntimeState&, const MissionDef*);
    mission_ui_update(w, g_runtime.rt, def);
}

void mission_runtime_select(const std::string& mission_id) {
    if (g_mission_db.by_id.count(mission_id)) {
        g_runtime.rt.mission_id = mission_id;
    }
}

MissionRuntimeState& mission_runtime_state() { return g_runtime.rt; }
const MissionDef* mission_runtime_current_def() { return current_def(); }

} // namespace ptv
