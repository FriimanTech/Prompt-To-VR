#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>

#include "mission_flow_shared.h"

namespace ptv {

class MissionFlowSystem : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        // Cache default interact binding in the star_map entity's tag for reference
        const std::string interact_btn = params.value(kInteractParamName, std::string("A_right"));
        mission_merge_tags(world, entity, {std::string("mission.interact:" ) + interact_btn});
    }

    void on_update(World& world, std::uint32_t entity, float /*dt*/) override {
        // Find player
        auto player_opt = mission_find_player_avatar(world);
        if (!player_opt.has_value()) return;
        const std::uint32_t player = *player_opt;

        // If params in binding say to accept directly (e.g., driven by wrist menu), handle quickly
        if (world.has<CMechanicBinding>(entity)) {
            const auto& bind = world.get<CMechanicBinding>(entity);
            for (const auto& entry : bind.entries) {
                if (entry.id == "generated.mission_flow_system" || entry.builtin_id == "generated.mission_flow_system") {
                    auto jp = mission_parse_params_json(entry);
                    if (jp.has_value()) {
                        const auto& j = *jp;
                        // Accept command by mission_id
                        if (j.contains("accept") && j.contains("mission_id") && j.at("accept").is_boolean() && j.at("accept").get<bool>()) {
                            const std::string mid = j.at("mission_id").get<std::string>();
                            // lookup mission
                            const auto& dir = mission_directory();
                            auto it = std::find_if(dir.begin(), dir.end(), [&](const MissionDef& d){ return d.id == mid; });
                            if (it != dir.end()) {
                                mission_accept_by_id(world, player, *it);
                                mission_push_objectives_to_hud(world, player);
                            }
                        }
                        // Objective update passthrough from other systems (e.g., rescue, combat)
                        if (j.contains("objective_update") && j.at("objective_update").is_object()) {
                            auto ou = j.at("objective_update");
                            std::string tag = ou.value("tag", std::string());
                            int delta = ou.value("delta", 0);
                            int target_if_zero = ou.value("target_if_zero", 0);
                            if (!tag.empty()) {
                                if (mission_update_objective(world, player, tag, delta, target_if_zero)) {
                                    mission_try_complete(world, player);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Proximity and interact gate for opening selection preview at the star_map
        bool near_star_map = false;
        if (world.has<CTransform>(entity)) {
            const auto& map_tr = world.get<CTransform>(entity).t;
            if (world.has<CTransform>(player)) {
                const auto& p_tr = world.get<CTransform>(player).t;
                const glm::vec3 d = glm::vec3(map_tr.position.x - p_tr.position.x,
                                              map_tr.position.y - p_tr.position.y,
                                              map_tr.position.z - p_tr.position.z);
                const float dist2 = glm::dot(d, d);
                near_star_map = dist2 <= (kInteractRadiusMeters * kInteractRadiusMeters);
            }
        }

        // If near and binding signals an interact press, set preview tags so wrist HUD can show a panel
        if (near_star_map && world.has<CMechanicBinding>(entity)) {
            const auto& bind = world.get<CMechanicBinding>(entity);
            for (const auto& entry : bind.entries) {
                if (entry.id == "generated.mission_flow_system" || entry.builtin_id == "generated.mission_flow_system") {
                    auto jp = mission_parse_params_json(entry);
                    if (jp.has_value()) {
                        const auto& j = *jp;
                        const bool interact_pressed = j.value("interact_pressed", false);
                        if (interact_pressed) {
                            // Publish a lightweight directory into player's tags for HUD/wrist menu
                            std::vector<std::string> t;
                            t.push_back("hud.mission_panel:Open");
                            const auto& dir = mission_directory();
                            for (const auto& d : dir) {
                                t.push_back(std::string("hud.mission.item:") + d.id);
                                t.push_back(std::string("hud.mission.name:") + d.id + ":" + d.name);
                                t.push_back(std::string("hud.mission.diff:") + d.id + ":" + std::to_string(d.difficulty));
                            }
                            mission_merge_tags(world, player, t);
                        }
                        // Wrist menu may set selected_mission_id and accept=true; handled above.
                    }
                }
            }
        }

        // Always keep HUD in sync if we carry state
        if (world.has<CMissionState>(player)) {
            mission_sync_hud_tags(world, player);
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Clear star map preview tags on detach
        if (auto player_opt = mission_find_player_avatar(world)) {
            const auto player = *player_opt;
            if (world.has<CTag>(player)) {
                auto keep = world.get<CTag>(player).tags;
                keep.erase(std::remove_if(keep.begin(), keep.end(), [](const std::string& s){
                    return s.rfind("hud.mission_panel", 0) == 0 || s.rfind("hud.mission.item:", 0) == 0 || s.rfind("hud.mission.name:", 0) == 0 || s.rfind("hud.mission.diff:", 0) == 0; }), keep.end());
                world.emplace_or_replace<CTag>(player, CTag{keep});
            }
        }
        // Keep any mission/objective state on player; do not wipe on detach
        (void)entity;
    }

    std::string id() const override { return "generated.mission_flow_system"; }
};

PTV_REGISTER_MECHANIC("generated.mission_flow_system", MissionFlowSystem)

} // namespace ptv
