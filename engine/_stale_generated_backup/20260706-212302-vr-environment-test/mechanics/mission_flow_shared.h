#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <chrono>

namespace ptv {

// Forward declarations for Save/Load API (from save_load_system exports)
// Implemented in that task; we only declare to link against them.
[[nodiscard]] nlohmann::json ptv_profile_get(const std::string& key);
void ptv_profile_set(const std::string& key, const nlohmann::json& value);
[[nodiscard]] nlohmann::json ptv_settings_get(const std::string& key);
void ptv_settings_set(const std::string& key, const nlohmann::json& value);
void ptv_save_now();
[[nodiscard]] std::string ptv_profile_current();

struct ObjectiveProgress {
    std::string tag;     // e.g. "enemies_defeated"
    int current{0};
    int target{1};
};

struct CObjectiveState {
    std::vector<ObjectiveProgress> current;   // active objectives
    std::vector<std::string> completed_tags;  // history for HUD/wrist
};

struct MissionDef {
    std::string id;
    std::string name;
    std::string summary;
    int difficulty{1};
    int reward_mastery_points{1};
    std::vector<ObjectiveProgress> objectives; // target counts pre-filled
    // Optional comfort variants:
    bool comfort_reduced_speed{false};
    float comfort_speed_scale{0.8f}; // 0.8 => 20% slower
    float comfort_horizon_lock{0.2f}; // abstract hint value 0..1
};

struct CMissionState {
    bool active{false};
    std::string mission_id;
    std::string mission_name;
    std::string mission_summary;
    int difficulty{1};
    int reward_mastery_points{0};
    bool comfort_reduced_speed{false};
    float comfort_speed_scale{1.0f};
    float comfort_horizon_lock{0.0f};
    bool completed{false};
    bool failed{false};
    // Timestamp bookkeeping (epoch ms) for analytics/future use
    std::uint64_t accepted_ms{0};
    std::uint64_t completed_ms{0};
};

// Shared constants and helpers
static constexpr const char* kInteractParamName = "interact_button"; // params default: "A_right"
static constexpr float kInteractRadiusMeters = 2.25f; // proximity to star_map to open panel

// Find primary player avatar entity (has tag "player_avatar").
std::optional<std::uint32_t> mission_find_player_avatar(World& world);
// Push current objectives to HUD/wrist by reflecting into player's CTag tags.
void mission_push_objectives_to_hud(World& world, std::uint32_t player_entity);
// Accept a mission by id, set CMissionState and seed CObjectiveState.
bool mission_accept_by_id(World& world, std::uint32_t player_entity, const MissionDef& def);
// Update an objective by tag; returns true if state changed.
bool mission_update_objective(World& world, std::uint32_t player_entity,
                              const std::string& tag, int delta, int target_if_zero = 0);
// Complete active mission if all objectives done; award rewards and autosave.
void mission_try_complete(World& world, std::uint32_t player_entity);

// Provide a small built-in mission directory for selection ring.
const std::vector<MissionDef>& mission_directory();

// Utility to append tags non-destructively into CTag on an entity.
void mission_merge_tags(World& world, std::uint32_t e, const std::vector<std::string>& add);

// Find an entity by string id safely (helper around world.find).
std::optional<std::uint32_t> mission_find_entity(World& world, const std::string& id);

// Lightweight ring-UI state we mirror via tags (so HUD/wrist can read without tight coupling).
// We encode as CTag entries like: "hud.mission:Active", "hud.mission.name:<name>",
// and for objectives: "hud.obj:<tag>:<current>/<target>".
void mission_sync_hud_tags(World& world, std::uint32_t player_entity);

// Parse helper for a CMechanicBinding entry's params_json into JSON.
std::optional<nlohmann::json> mission_parse_params_json(const ptv::CMechanicBinding::Entry& entry);

} // namespace ptv
