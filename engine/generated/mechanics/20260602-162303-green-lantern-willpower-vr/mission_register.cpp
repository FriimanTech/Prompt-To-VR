#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <optional>

#include "mission_shared.h"

namespace ptv {

class MissionSystemMechanic : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        self_ = entity;
        // Dependency contracts (prefixes) from completed tasks
        ui_page_register_prefix_ = "ui.page.register:";   // from hud_and_wrist_menu
        ui_toast_prefix_ = "ui.toast:";                   // from hud_and_wrist_menu
        save_section_push_prefix_ = "save.section.push:"; // from save_persistence
        save_request_prefix_ = "save.request:";           // from save_persistence
        mission_complete_prefix_ = "mission.complete:";   // consumed by save_persistence/progression
        objective_progress_prefix_ = "objective.progress:"; // generic objective updates
        portal_travel_request_prefix_ = "portal.travel.request:"; // exposed for portal_travel module
        ai_difficulty_prefix_ = "ai.difficulty.profile:"; // hint to AI packs

        // Parse mission DB and medal thresholds from binding params
        if (params.contains("missions")) {
            mission_util::parse_missions(params.at("missions"), missions_);
        }
        if (params.contains("medal_thresholds")) {
            thresholds_ = mission_util::parse_medal_thresholds(params.at("medal_thresholds"));
        }

        // Register wrist page with HUD
        mission_util::ensure_wrist_page_registered(world, entity, ui_page_register_prefix_);

        // Optional: preselect a mission via params
        pending_select_id_ = params.value("preselect_mission_id", std::string{});
        pending_difficulty_ = params.value("preselect_difficulty", std::string{"normal"});
        if (!pending_select_id_.empty()) want_confirm_ = true; // auto-confirm in tests if preselected
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (entity != self_) self_ = entity; // safety

        // Read per-frame control intents from our own CMechanicBinding entry params_json
        read_binding_params(world, entity);

        // Handle a new selection/confirmation
        handle_selection_and_launch(world, entity);

        // If a mission is running, tick timer and collect objective updates from tags
        if (rt_.in_progress && !rt_.finished && !rt_.failed) {
            rt_.elapsed_s += dt;
            const auto* def = get_current_def();
            if (def) {
                mission_util::tick_objectives_from_tags(world, rt_, *def, objective_progress_prefix_, self_);
            }
        }

        // If finished, evaluate medal and persist
        if (rt_.finished) {
            const auto* def = get_current_def();
            if (def) {
                mission_util::maybe_award_medal_and_persist(world, entity, *def, rt_, thresholds_,
                    save_section_push_prefix_, save_request_prefix_, ui_toast_prefix_, mission_complete_prefix_);
            }
        }

        // Opportunistic UI hinting
        mission_util::update_wrist_status(world, entity, get_current_def(), rt_, ui_toast_prefix_, 2.0f);
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.mission_system"; }

private:
    // Read intents from our binding entry if present.
    void read_binding_params(World& world, std::uint32_t entity) {
        pending_select_id_.clear();
        // We allow tests/other modules to drive this via binding params OR via tags on this entity
        if (world.has<CMechanicBinding>(entity)) {
            const auto& mb = world.get<CMechanicBinding>(entity);
            for (const auto& entry : mb.entries) {
                if (entry.id == id() || entry.builtin_id == id()) {
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if (!j.is_object()) continue;
                    if (j.contains("select_mission_id")) {
                        pending_select_id_ = j.value("select_mission_id", std::string{});
                    }
                    if (j.contains("difficulty")) {
                        pending_difficulty_ = j.value("difficulty", std::string{"normal"});
                    }
                    want_confirm_ = j.value("confirm", want_confirm_);
                    portal_activated_ = j.value("portal_activated", portal_activated_);
                    // Objective synthetic injection for tests
                    if (j.contains("objective_progress")) {
                        auto pj = j.at("objective_progress");
                        if (pj.is_object()) {
                            nlohmann::json m; m["id"] = pj.value("id", ""); m["delta"] = pj.value("delta", 0); m["state"] = pj.value("state", "update");
                            mission_util::push_tag(world, entity, mission_util::make_tag(objective_progress_prefix_, m));
                        }
                    }
                }
            }
        }
        // Also allow external console to poke via tags: mission.select:{id, difficulty, confirm:true}
        if (world.has<CTag>(entity)) {
            auto& tags = world.get<CTag>(entity).tags;
            for (auto it = tags.begin(); it != tags.end(); ) {
                const std::string& s = *it;
                const std::string selectPrefix = "mission.select:";
                const std::string confirmPrefix = "mission.confirm:";
                const std::string portalPrefix = "portal.activated:";
                if (s.rfind(selectPrefix, 0) == 0) {
                    auto j = nlohmann::json::parse(s.substr(selectPrefix.size()), nullptr, false);
                    if (j.is_object()) {
                        pending_select_id_ = j.value("id", pending_select_id_);
                        pending_difficulty_ = j.value("difficulty", pending_difficulty_);
                    }
                    it = tags.erase(it);
                    continue;
                } else if (s.rfind(confirmPrefix, 0) == 0) {
                    want_confirm_ = true; it = tags.erase(it); continue;
                } else if (s.rfind(portalPrefix, 0) == 0) {
                    portal_activated_ = true; it = tags.erase(it); continue;
                } else {
                    ++it;
                }
            }
        }
    }

    void handle_selection_and_launch(World& world, std::uint32_t entity) {
        // Selection phase
        if (!pending_select_id_.empty()) {
            auto it = missions_.find(pending_select_id_);
            if (it != missions_.end()) {
                current_mission_id_ = pending_select_id_;
                rt_.has_selection = true;
                rt_.selected_mission_id = current_mission_id_;
                if (!pending_difficulty_.empty()) rt_.selected_difficulty = pending_difficulty_;
                mission_util::reset_runtime_for_mission(it->second, rt_);
                // AI scaling hint for other systems
                nlohmann::json dif; dif["tier"] = rt_.selected_difficulty; dif["scale"] = tier_ai_scale(it->second, rt_.selected_difficulty);
                mission_util::push_tag(world, entity, mission_util::make_tag(ai_difficulty_prefix_, dif));
                // Toast
                nlohmann::json t; t["text"] = std::string("Mission Selected: ") + current_mission_id_ + " (" + rt_.selected_difficulty + ")"; t["style"] = "info";
                mission_util::push_tag(world, entity, mission_util::make_tag(ui_toast_prefix_, t));
            }
            pending_select_id_.clear();
        }
        // Launch once confirmed and portal is activated
        if (rt_.has_selection && want_confirm_ && portal_activated_ && !rt_.in_progress) {
            const MissionDef* def = get_current_def();
            if (def) {
                // Ask portal system to begin travel; if portal module isn't present yet, tests still observe our intent via tag
                nlohmann::json travel; travel["mission_id"] = def->id; travel["difficulty"] = rt_.selected_difficulty; travel["scenes"] = def->scenes;
                mission_util::push_tag(world, entity, mission_util::make_tag(portal_travel_request_prefix_, travel));
                // Start runtime timer immediately (MVP)
                rt_.in_progress = true;
                rt_.elapsed_s = 0.0f;
                want_confirm_ = false;
                portal_activated_ = false;
                // Toast
                nlohmann::json t; t["text"] = std::string("Deploying: ") + def->id; t["style"] = "info";
                mission_util::push_tag(world, entity, mission_util::make_tag(ui_toast_prefix_, t));
            }
        }
    }

    const MissionDef* get_current_def() const {
        auto it = missions_.find(current_mission_id_);
        if (it == missions_.end()) return nullptr;
        return &it->second;
    }

    float tier_ai_scale(const MissionDef& def, const std::string& tierId) const {
        for (const auto& t : def.tiers) if (t.id == tierId) return t.ai_scale;
        return 1.0f;
    }

private:
    std::uint32_t self_{0};

    // Protocol prefixes
    std::string ui_page_register_prefix_;
    std::string ui_toast_prefix_;
    std::string save_section_push_prefix_;
    std::string save_request_prefix_;
    std::string mission_complete_prefix_;
    std::string objective_progress_prefix_;
    std::string portal_travel_request_prefix_;
    std::string ai_difficulty_prefix_;

    std::unordered_map<std::string, MissionDef> missions_;
    MedalThresholds thresholds_{};

    // Selection/launch intents
    std::string pending_select_id_;
    std::string pending_difficulty_{"normal"};
    bool want_confirm_{false};
    bool portal_activated_{false};

    // Active mission runtime
    std::string current_mission_id_;
    MissionRuntimeState rt_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.mission_system", ptv::MissionSystemMechanic)
