#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>
#include <vector>
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

// From other files in this mechanic
extern MissionDB g_mission_db; // mission_db.cpp
void mission_db_ensure_built_from(const nlohmann::json& params); // mission_db.cpp
void mission_runtime_begin_selected(World&);
void mission_runtime_cancel();
void mission_runtime_tick(World&, float dt);
void mission_runtime_select(const std::string& mission_id);
MissionRuntimeState& mission_runtime_state();
const MissionDef* mission_runtime_current_def();
std::string evaluate_and_record_medal(const MissionDef&, MissionRuntimeState&); // medals_scoring.cpp

namespace {

static bool tag_has(const std::vector<std::string>& tags, const std::string& needle) {
    return std::find(tags.begin(), tags.end(), needle) != tags.end();
}

static std::optional<std::string> tag_get_value(const std::vector<std::string>& tags, const std::string& key_prefix) {
    const std::string p = key_prefix + ":";
    for (const auto& t : tags) if (t.rfind(p, 0) == 0) return t.substr(p.size());
    return std::nullopt;
}

static void ensure_component(World& w, std::uint32_t e) {
    if (!w.has<CTag>(e)) w.emplace_or_replace<CTag>(e, CTag{});
}

static void set_tag_kv(World& w, std::uint32_t e, const std::string& k, const std::string& v) {
    ensure_component(w, e);
    auto& tags = w.get<CTag>(e).tags;
    const std::string kv = k + ":" + v;
    tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){return s.rfind(k+":",0)==0;}), tags.end());
    tags.push_back(kv);
}

static void try_handle_console_interact(World& w, std::uint32_t e) {
    // If console has tag ui:confirm_mission:<id> set by the test harness, accept and arm portal.
    if (!w.has<CTag>(e)) return;
    auto& tags = w.get<CTag>(e).tags;
    if (auto sel = tag_get_value(tags, "ui:select_mission")) {
        mission_runtime_select(*sel);
        set_tag_kv(w, e, "console:selection", *sel);
        // clear selection tag to emulate one-shot
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){return s.rfind("ui:select_mission:",0)==0;}), tags.end());
    }
    if (auto conf = tag_get_value(tags, "ui:confirm_mission")) {
        mission_runtime_select(*conf);
        mission_runtime_begin_selected(w);
        set_tag_kv(w, e, "console:confirmed", *conf);
        // one-shot clear
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){return s.rfind("ui:confirm_mission:",0)==0;}), tags.end());
    }
    if (tag_has(tags, "ui:cancel_mission")) {
        mission_runtime_cancel();
        tags.erase(std::remove(tags.begin(), tags.end(), "ui:cancel_mission"), tags.end());
    }
}

static void publish_difficulty_tags(World& w, const std::string& tier) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ee : view) {
        auto& tags = reg.get<CTag>(ee).tags;
        bool is_ai = std::find(tags.begin(), tags.end(), "enemy") != tags.end();
        if (!is_ai) continue;
        const std::string key = "ai:difficulty";
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){return s.rfind(key+":",0)==0;}), tags.end());
        tags.push_back(key + ":" + tier);
    }
}

} // namespace

class MissionSystem : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        // Build the mission DB once from any console's params (prefer first with definitions)
        if (!db_built_) {
            mission_db_ensure_built_from(params);
            db_built_ = true;
        }
        // Allow console-local difficulty hint: params.value("difficulty", "normal")
        selected_difficulty_ = params.value("difficulty", std::string{"normal"});
        mission_runtime_state().selected_difficulty = selected_difficulty_;
        publish_difficulty_tags(w, selected_difficulty_);
        // Tag console for UI discoverability
        set_tag_kv(w, e, "console:mission_system", "1");
        // Advertise available missions (first 4 for brevity) as tags the UI prompt can read
        int count = 0;
        for (const auto& kv : g_mission_db.by_id) {
            if (count++ >= 4) break;
            set_tag_kv(w, e, std::string("console:mission:")+std::to_string(count), kv.first);
        }
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        try_handle_console_interact(w, e);
        mission_runtime_tick(w, dt);

        // If mission just finished this frame, evaluate medals and push reward tags.
        auto* def = mission_runtime_current_def();
        auto& rt = mission_runtime_state();
        if (def) {
            const bool finished_now = (!rt.mission_active && finished_guard_ == false && rt.current_obj_index >= 0);
            if (finished_now) {
                finished_guard_ = true; // prevent multi-award
                const std::string medal = evaluate_and_record_medal(*def, rt);
                if (!medal.empty()) {
                    // Broadcast simple reward tag on console to simulate progression hook
                    set_tag_kv(w, e, "mission:medal_awarded", medal);
                }
                // Also set generic completion tag
                set_tag_kv(w, e, "mission:completed", def->id);
            }
            if (rt.mission_active) finished_guard_ = false; // reset guard when re-armed
        }
    }

    void on_detach(World& w, std::uint32_t /*e*/) override {
        (void)w; // nothing
    }

    std::string id() const override { return "generated.mission_system"; }

private:
    bool db_built_{false};
    bool finished_guard_{false};
    std::string selected_difficulty_{"normal"};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.mission_system", ptv::MissionSystem)
