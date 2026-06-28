#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <string>
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

namespace hud_helpers { bool hud_get_settings_json(std::string&); }

namespace {

static void add_or_replace_tag(std::vector<std::string>& tags, const std::string& key, const std::string& value) {
    const std::string full = key + ":" + value;
    // remove previous entries with same key prefix
    tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){return s.rfind(key + ":", 0) == 0;}), tags.end());
    tags.push_back(full);
}

static void push_hud_mission_lines(World& w, const MissionRuntimeState& rt, const MissionDef* def) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        auto& tags = reg.get<CTag>(e).tags;
        // Only write to entities likely to be HUD anchors.
        bool is_hudish = std::any_of(tags.begin(), tags.end(), [](const std::string& t){return t=="hud" || t=="hud_anchor";});
        if (!is_hudish) continue;
        add_or_replace_tag(tags, "hud:mission:title", def ? def->id : std::string("no_mission"));
        add_or_replace_tag(tags, "hud:mission:active", rt.mission_active ? "1" : "0");
        add_or_replace_tag(tags, "hud:mission:elapsed", std::to_string((int)rt.elapsed_s));
        if (def && rt.current_obj_index >= 0 && rt.current_obj_index < (int)def->chain.size()) {
            const auto& obj = def->chain[rt.current_obj_index];
            add_or_replace_tag(tags, "hud:mission:obj", obj.description.empty()?obj.id:obj.description);
        } else {
            add_or_replace_tag(tags, "hud:mission:obj", "-");
        }
        add_or_replace_tag(tags, "hud:mission:score", std::to_string((int)rt.score));
        if (!rt.last_awarded_medal.empty()) add_or_replace_tag(tags, "hud:mission:medal", rt.last_awarded_medal);
    }
}

static void push_wrist_mission_lines(World& w, const MissionRuntimeState& rt, const MissionDef* def) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        auto& tags = reg.get<CTag>(e).tags;
        bool is_hand = std::any_of(tags.begin(), tags.end(), [](const std::string& t){return t=="player_avatar" || t=="right_hand";});
        if (!is_hand) continue;
        add_or_replace_tag(tags, "wrist:mission:title", def ? def->id : std::string("no_mission"));
        add_or_replace_tag(tags, "wrist:mission:elapsed", std::to_string((int)rt.elapsed_s));
        add_or_replace_tag(tags, "wrist:mission:score", std::to_string((int)rt.score));
    }
}

} // namespace

void mission_ui_update(World& w, const MissionRuntimeState& rt, const MissionDef* def) {
    std::string hud_json; (void)hud_helpers::hud_get_settings_json(hud_json); // touch dependency symbol
    push_hud_mission_lines(w, rt, def);
    push_wrist_mission_lines(w, rt, def);
}

} // namespace ptv
