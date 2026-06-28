#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <algorithm>

namespace ptv {

// Forward declares (matching symbols exported by deps)
namespace mission_api {
    std::string current_mission_id();
    bool mission_in_progress();
}

// --- Small helpers ---
bool PortalTravel::j_get_bool(const nlohmann::json& j, const char* k, bool def_v) {
    return j.is_object() && j.contains(k) ? j.at(k).get<bool>() : def_v;
}
int PortalTravel::j_get_int(const nlohmann::json& j, const char* k, int def_v) {
    return j.is_object() && j.contains(k) ? j.at(k).get<int>() : def_v;
}
std::string PortalTravel::j_get_str(const nlohmann::json& j, const char* k, const std::string& def_v) {
    return j.is_object() && j.contains(k) ? j.at(k).get<std::string>() : def_v;
}

static void write_ctag_list(std::vector<std::string>& v, const std::string& key, bool present) {
    auto it = std::find(v.begin(), v.end(), key);
    if (present) {
        if (it == v.end()) v.push_back(key);
    } else {
        if (it != v.end()) v.erase(it);
    }
}

void PortalTravel::ensure_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) {
        CTag t; t.tags = {tag};
        w.emplace_or_replace<CTag>(e, std::move(t));
        return;
    }
    auto& tags = w.get<CTag>(e).tags;
    write_ctag_list(tags, tag, true);
}

void PortalTravel::drop_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) return;
    auto& tags = w.get<CTag>(e).tags;
    write_ctag_list(tags, tag, false);
}

void PortalTravel::ensure_status_tag(World& w, std::uint32_t e, const std::string& status) {
    // Normalize by removing any previous portal_status:* tags, then add the new one.
    if (!w.has<CTag>(e)) {
        CTag t; t.tags = {std::string("portal_status:") + status};
        w.emplace_or_replace<CTag>(e, std::move(t));
        return;
    }
    auto& tags = w.get<CTag>(e).tags;
    // remove old statuses
    for (auto it = tags.begin(); it != tags.end();) {
        if (it->rfind("portal_status:", 0) == 0) it = tags.erase(it); else ++it;
    }
    tags.push_back(std::string("portal_status:") + status);
}

// Binding helpers (implemented here; no extra headers allowed)
void PortalTravel::ensure_practice_spawner_binding(World& w, std::uint32_t e, int max_alive) {
    CMechanicBinding mb;
    if (w.has<CMechanicBinding>(e)) {
        mb = w.get<CMechanicBinding>(e);
    }
    bool found = false;
    for (auto& en : mb.entries) {
        if (en.id == "practice_drones" && en.builtin_id == "builtin.spawner") { found = true; break; }
    }
    if (!found) {
        CMechanicBinding::Entry en{};
        en.id = "practice_drones";
        en.builtin_id = "builtin.spawner";
        nlohmann::json pj{
            {"spawn_kind", "drone_practice"},
            {"max_alive", std::max(0, max_alive)},
            {"rate_per_s", 0.5},
            {"notes", "Spawned while in Sector Travel Corridor for target practice."}
        };
        en.params_json = pj.dump();
        mb.entries.push_back(std::move(en));
        w.emplace_or_replace<CMechanicBinding>(e, std::move(mb));
    }
}

void PortalTravel::remove_practice_spawner_binding(World& w, std::uint32_t e) {
    if (!w.has<CMechanicBinding>(e)) return;
    auto mb = w.get<CMechanicBinding>(e);
    auto& v = mb.entries;
    v.erase(std::remove_if(v.begin(), v.end(), [](const CMechanicBinding::Entry& en){
        return en.id == "practice_drones" && en.builtin_id == "builtin.spawner";
    }), v.end());
    w.emplace_or_replace<CMechanicBinding>(e, std::move(mb));
}

// --- Lifecycle ---
void PortalTravel::on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) {
    practice_mode_ = j_get_bool(params, "practice_mode", true);
    practice_max_alive_ = j_get_int(params, "practice_max_alive", 4);
    arm_time_s_  = std::max(0.1f, static_cast<float>(j_get_bool(params, "fast_arm", false) ? 0.2f : 0.5f));
    fade_time_s_ = std::max(0.2f, static_cast<float>(j_get_bool(params, "fast_fade", false) ? 0.25f : 0.6f));
    corridor_min_time_s_ = std::max(0.5f, static_cast<float>(params.value("corridor_min_time_s", 3.0)));
    target_scene_ = j_get_str(params, "target_scene", "");

    // Set initial status for acceptance hooks.
    ensure_status_tag(world, entity, "IdleAtLaunch");
    ensure_tag(world, entity, "hud:portal_status_chip"); // UI chip presence (registered via HUD system)

    // Cache a mission label if any
    mission_name_cache_ = mission_api::current_mission_id();
    if (!mission_name_cache_.empty()) {
        ensure_tag(world, entity, std::string("mission:") + mission_name_cache_);
    }
}

void PortalTravel::on_update(World& world, std::uint32_t entity, float dt) {
    // Read per-frame control inputs from params_json of our own binding entry (test harness drives it)
    bool interact_pressed = false; // "A" on gate console at launch
    bool b_pressed = false;        // Cancel while in corridor
    // Scan our binding entry (if any) to pull transient inputs.
    if (world.has<CMechanicBinding>(entity)) {
        const auto& mb = world.get<CMechanicBinding>(entity);
        for (const auto& en : mb.entries) {
            if (en.id == id()) {
                auto j = nlohmann::json::parse(en.params_json, nullptr, false);
                if (j.is_object()) {
                    interact_pressed = j.value("interact_pressed", false);
                    b_pressed = j.value("b_pressed", false);
                    // allow dynamic overrides
                    if (j.contains("practice_mode")) practice_mode_ = j.value("practice_mode", practice_mode_);
                    if (j.contains("practice_max_alive")) practice_max_alive_ = j.value("practice_max_alive", practice_max_alive_);
                    if (j.contains("target_scene")) target_scene_ = j.value("target_scene", target_scene_);
                }
                break;
            }
        }
    }

    t_ += dt;

    // Keep HUD chip alive and status visible
    ensure_tag(world, entity, "hud:portal_status_chip");

    switch (phase_) {
    case Phase::IdleAtLaunch: {
        ensure_status_tag(world, entity, "IdleAtLaunch");
        // Update mission label if mission system has one.
        {
            auto mid = mission_api::current_mission_id();
            if (mid != mission_name_cache_) {
                mission_name_cache_ = mid;
                if (!mid.empty()) ensure_tag(world, entity, std::string("mission:") + mid);
            }
        }
        // Gate VFX toggle tag
        if (!mission_name_cache_.empty()) ensure_tag(world, entity, "portal_vfx:standby");
        else drop_tag(world, entity, "portal_vfx:standby");

        if (interact_pressed && !mission_name_cache_.empty()) {
            phase_ = Phase::Arming;
            t_ = 0.0f;
            // Visual cue
            drop_tag(world, entity, "portal_vfx:standby");
            ensure_tag(world, entity, "portal_vfx:arming");
            ensure_status_tag(world, entity, "Arming");
        }
        break; }
    case Phase::Arming: {
        ensure_status_tag(world, entity, "Arming");
        if (t_ >= arm_time_s_) {
            phase_ = Phase::FadingOut;
            t_ = 0.0f;
            drop_tag(world, entity, "portal_vfx:arming");
            ensure_tag(world, entity, "screen_fade:out");
            ensure_status_tag(world, entity, "FadingOut");
        }
        break; }
    case Phase::FadingOut: {
        if (t_ >= fade_time_s_) {
            // Enter corridor pseudo-scene (mask loading via tags/bindings)
            phase_ = Phase::InCorridor;
            t_ = 0.0f;
            drop_tag(world, entity, "screen_fade:out");
            ensure_tag(world, entity, "scene:sector_travel_corridor");
            ensure_status_tag(world, entity, "InCorridor");

            // Optional practice spawner while travelling
            if (practice_mode_ && !practice_bound_) {
                ensure_practice_spawner_binding(world, entity, practice_max_alive_);
                practice_bound_ = true;
            }
        }
        break; }
    case Phase::InCorridor: {
        ensure_status_tag(world, entity, "InCorridor");
        ensure_tag(world, entity, "portal_vfx:corridor_active");

        if (b_pressed) {
            // Emergency cancel: return to launch platform
            phase_ = Phase::Cancelling;
            t_ = 0.0f;
            ensure_status_tag(world, entity, "Cancelling");
            ensure_tag(world, entity, "screen_fade:out");
            break;
        }

        if (t_ >= corridor_min_time_s_) {
            // Arrive: handoff to mission scene (announce via tags)
            phase_ = Phase::Arrived;
            t_ = 0.0f;
            ensure_status_tag(world, entity, "Arrived");
            drop_tag(world, entity, "portal_vfx:corridor_active");
            // Mark pending mission scene; default from params or infer from mission id
            std::string dest = target_scene_;
            if (dest.empty()) {
                // Simple heuristic mapping for tests: choose by mission id prefix
                const auto mid = mission_name_cache_;
                if (mid.find("training") != std::string::npos) dest = "training_arena";
                else if (mid.find("city") != std::string::npos) dest = "alien_city_attack";
                else if (mid.find("derelict") != std::string::npos) dest = "derelict_space_station";
                else dest = "open_orbital_battlefield";
            }
            ensure_tag(world, entity, std::string("pending_scene:") + dest);
            ensure_tag(world, entity, "screen_fade:in");
            // Tear down practice spawner
            if (practice_bound_) { remove_practice_spawner_binding(world, entity); practice_bound_ = false; }
        }
        break; }
    case Phase::Cancelling: {
        if (t_ >= fade_time_s_ * 0.5f) {
            // Restore to launch platform
            drop_tag(world, entity, "scene:sector_travel_corridor");
            drop_tag(world, entity, "portal_vfx:corridor_active");
            ensure_tag(world, entity, "scene:launch_platform");
            ensure_tag(world, entity, "screen_fade:in");
            ensure_status_tag(world, entity, "IdleAtLaunch");
            if (practice_bound_) { remove_practice_spawner_binding(world, entity); practice_bound_ = false; }
            phase_ = Phase::IdleAtLaunch;
            t_ = 0.0f;
        }
        break; }
    case Phase::Arrived: {
        // Idle here; external mission loader can observe pending_scene:* and swap scene.
        // Keep fade tag for one frame, then clear.
        if (t_ > 0.2f) drop_tag(world, entity, "screen_fade:in");
        break; }
    }
}

void PortalTravel::on_detach(World& world, std::uint32_t entity) {
    // Clean up visual/status tags we may have set
    drop_tag(world, entity, "portal_vfx:standby");
    drop_tag(world, entity, "portal_vfx:arming");
    drop_tag(world, entity, "portal_vfx:corridor_active");
    drop_tag(world, entity, "screen_fade:out");
    drop_tag(world, entity, "screen_fade:in");
    drop_tag(world, entity, "scene:sector_travel_corridor");
    // leave mission:*, pending_scene:* for external systems if desired
    if (practice_bound_) { remove_practice_spawner_binding(world, entity); practice_bound_ = false; }
}

} // namespace ptv
