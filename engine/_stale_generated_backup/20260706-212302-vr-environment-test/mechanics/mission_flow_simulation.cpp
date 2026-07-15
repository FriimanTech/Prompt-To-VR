#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <chrono>

#include "mission_flow_shared.h"

namespace ptv {

static std::uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::optional<std::uint32_t> mission_find_player_avatar(World& world) {
    auto& reg = world.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        const auto& tags = view.get<CTag>(e).tags;
        if (std::find(tags.begin(), tags.end(), std::string("player_avatar")) != tags.end()) {
            return static_cast<std::uint32_t>(e);
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> mission_find_entity(World& world, const std::string& id) {
    auto entt_e = world.find(id);
    if (entt_e == entt::null) return std::nullopt;
    return static_cast<std::uint32_t>(entt_e);
}

void mission_merge_tags(World& world, std::uint32_t e, const std::vector<std::string>& add) {
    std::vector<std::string> merged;
    if (world.has<CTag>(e)) merged = world.get<CTag>(e).tags;
    for (const auto& t : add) {
        if (std::find(merged.begin(), merged.end(), t) == merged.end()) merged.push_back(t);
    }
    world.emplace_or_replace<CTag>(e, CTag{merged});
}

static void hud_encode_basic(World& world, std::uint32_t player_entity, const CMissionState& ms) {
    std::vector<std::string> t;
    t.push_back(ms.active ? "hud.mission:Active" : "hud.mission:Inactive");
    t.push_back(std::string("hud.mission.id:") + ms.mission_id);
    t.push_back(std::string("hud.mission.name:") + ms.mission_name);
    t.push_back(std::string("hud.mission.diff:") + std::to_string(ms.difficulty));
    if (ms.completed) t.push_back("hud.mission:Completed");
    if (ms.failed) t.push_back("hud.mission:Failed");
    mission_merge_tags(world, player_entity, t);
}

void mission_sync_hud_tags(World& world, std::uint32_t player_entity) {
    if (!world.has<CObjectiveState>(player_entity)) return;
    const auto& os = world.get<CObjectiveState>(player_entity);

    // Clear previous objective hud.* tags by rebuilding a fresh CTag set that keeps non-hud entries.
    std::vector<std::string> keep;
    if (world.has<CTag>(player_entity)) {
        for (const auto& s : world.get<CTag>(player_entity).tags) {
            if (s.rfind("hud.obj:", 0) != 0 && s.rfind("hud.mission", 0) != 0) keep.push_back(s);
        }
    }

    // Add current mission basic if available
    if (world.has<CMissionState>(player_entity)) {
        const auto& ms = world.get<CMissionState>(player_entity);
        std::vector<std::string> tmp = keep; // start from keep
        // Temporarily set to player; we'll append below then emplace
        world.emplace_or_replace<CTag>(player_entity, CTag{tmp});
        hud_encode_basic(world, player_entity, ms);
        // Fetch back the merged for appending objectives
        keep = world.get<CTag>(player_entity).tags;
        // Remove any hud.* duplicates before re-adding below
        keep.erase(std::remove_if(keep.begin(), keep.end(), [](const std::string& s){
            return s.rfind("hud.obj:", 0) == 0; }), keep.end());
    }

    // Build new hud.obj entries
    std::vector<std::string> hud;
    for (const auto& obj : os.current) {
        hud.push_back(std::string("hud.obj:") + obj.tag + ":" + std::to_string(obj.current) + "/" + std::to_string(obj.target));
    }
    for (const auto& tag : os.completed_tags) {
        hud.push_back(std::string("hud.obj.done:") + tag);
    }
    keep.insert(keep.end(), hud.begin(), hud.end());
    world.emplace_or_replace<CTag>(player_entity, CTag{keep});
}

bool mission_accept_by_id(World& world, std::uint32_t player_entity, const MissionDef& def) {
    CMissionState ms;
    ms.active = true;
    ms.mission_id = def.id;
    ms.mission_name = def.name;
    ms.mission_summary = def.summary;
    ms.difficulty = def.difficulty;
    ms.reward_mastery_points = def.reward_mastery_points;
    ms.comfort_reduced_speed = def.comfort_reduced_speed;
    ms.comfort_speed_scale = def.comfort_speed_scale;
    ms.comfort_horizon_lock = def.comfort_horizon_lock;
    ms.accepted_ms = now_ms();

    // Seed objective state
    CObjectiveState os;
    os.current = def.objectives;

    world.emplace_or_replace<CMissionState>(player_entity, ms);
    world.emplace_or_replace<CObjectiveState>(player_entity, os);

    // Mirror into HUD tags
    mission_sync_hud_tags(world, player_entity);

    // Persist active mission into profile and autosave
    nlohmann::json j = nlohmann::json::object();
    j["active"] = true;
    j["mission_id"] = def.id;
    j["objectives"] = nlohmann::json::array();
    for (const auto& o : def.objectives) {
        nlohmann::json jo;
        jo["tag"] = o.tag;
        jo["current"] = o.current;
        jo["target"] = o.target;
        j["objectives"].push_back(jo);
    }
    ptv_profile_set("active_mission", j);
    ptv_save_now();

    // Telemetry placeholder (no telemetry system available)
    // TODO: emit telemetry event "mission.accepted" with mission_id

    return true;
}

bool mission_update_objective(World& world, std::uint32_t player_entity,
                              const std::string& tag, int delta, int target_if_zero) {
    if (!world.has<CObjectiveState>(player_entity)) return false;
    auto os = world.get<CObjectiveState>(player_entity); // copy, we'll write back

    bool changed = false;
    for (auto& obj : os.current) {
        if (obj.tag == tag) {
            int before = obj.current;
            obj.current = std::max(0, obj.current + delta);
            if (obj.target == 0 && target_if_zero > 0) obj.target = target_if_zero;
            if (obj.current >= obj.target && obj.target > 0) {
                obj.current = obj.target;
                // Move to completed if not already
                if (std::find(os.completed_tags.begin(), os.completed_tags.end(), tag) == os.completed_tags.end()) {
                    os.completed_tags.push_back(tag);
                }
            }
            changed = (obj.current != before) || changed;
        }
    }

    // Remove completed ones from current list
    os.current.erase(std::remove_if(os.current.begin(), os.current.end(), [&](const ObjectiveProgress& o){
        return std::find(os.completed_tags.begin(), os.completed_tags.end(), o.tag) != os.completed_tags.end();
    }), os.current.end());

    if (changed) {
        world.emplace_or_replace<CObjectiveState>(player_entity, os);
        mission_sync_hud_tags(world, player_entity);
    }

    // Reflect into profile (lightweight persistence of progress)
    if (changed) {
        auto prof = ptv_profile_get("active_mission");
        if (prof.is_object()) {
            // Rewrite objectives array
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& o : os.current) {
                nlohmann::json jo;
                jo["tag"] = o.tag;
                jo["current"] = o.current;
                jo["target"] = o.target;
                arr.push_back(jo);
            }
            prof["objectives"] = arr;
            prof["completed_tags"] = os.completed_tags;
            ptv_profile_set("active_mission", prof);
        }
    }

    return changed;
}

void mission_try_complete(World& world, std::uint32_t player_entity) {
    if (!world.has<CMissionState>(player_entity) || !world.has<CObjectiveState>(player_entity)) return;
    auto ms = world.get<CMissionState>(player_entity);
    auto os = world.get<CObjectiveState>(player_entity);
    if (!ms.active || ms.completed || ms.failed) return;

    const bool all_done = os.current.empty();
    if (!all_done) return;

    ms.completed = true;
    ms.active = false;
    ms.completed_ms = now_ms();
    world.emplace_or_replace<CMissionState>(player_entity, ms);

    // Award mastery points in profile
    auto prof = ptv_profile_get("progression");
    if (!prof.is_object()) prof = nlohmann::json::object();
    int points = 0;
    if (prof.contains("mastery_points") && prof["mastery_points"].is_number_integer()) points = prof["mastery_points"].get<int>();
    points += std::max(0, ms.reward_mastery_points);
    prof["mastery_points"] = points;
    ptv_profile_set("progression", prof);

    // Clear active_mission and write a debrief blob
    nlohmann::json debrief = nlohmann::json::object();
    debrief["mission_id"] = ms.mission_id;
    debrief["reward_mastery_points"] = ms.reward_mastery_points;
    debrief["completed_tags"] = os.completed_tags;
    ptv_profile_set("last_debrief", debrief);
    ptv_profile_set("active_mission", nlohmann::json::object());

    ptv_save_now();

    // Update HUD tags to show completion
    mission_sync_hud_tags(world, player_entity);
}

static std::vector<MissionDef> g_directory; // filled once on first call

const std::vector<MissionDef>& mission_directory() {
    if (!g_directory.empty()) return g_directory;

    // Built-in sample missions
    MissionDef m1;
    m1.id = "alien_city_rescue";
    m1.name = "Alien City Under Attack";
    m1.summary = "Evacuate civilians and neutralize raiders.";
    m1.difficulty = 2;
    m1.reward_mastery_points = 3;
    m1.objectives = { {"civilians_rescued", 0, 8}, {"enemies_defeated", 0, 12} };
    m1.comfort_reduced_speed = true; m1.comfort_speed_scale = 0.85f; m1.comfort_horizon_lock = 0.35f;

    MissionDef m2;
    m2.id = "derelict_station_clear";
    m2.name = "Derelict Station Sweep";
    m2.summary = "Clear hostile drones and secure the station core.";
    m2.difficulty = 3;
    m2.reward_mastery_points = 4;
    m2.objectives = { {"drones_destroyed", 0, 15}, {"core_secured", 0, 1} };

    MissionDef m3;
    m3.id = "oa_defense";
    m3.name = "Oa Perimeter Defense";
    m3.summary = "Hold the line against incoming waves.";
    m3.difficulty = 4;
    m3.reward_mastery_points = 5;
    m3.objectives = { {"waves_completed", 0, 3} };

    g_directory = {m1, m2, m3};
    return g_directory;
}

std::optional<nlohmann::json> mission_parse_params_json(const ptv::CMechanicBinding::Entry& entry) {
    if (entry.params_json.empty()) return std::nullopt;
    auto j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return std::nullopt;
    return j;
}

void mission_push_objectives_to_hud(World& world, std::uint32_t player_entity) {
    mission_sync_hud_tags(world, player_entity);
}

} // namespace ptv
