#pragma once

#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace ptv {

// Forward-declare Save/Load API from dependency: save_load_system
// Signatures are designed to be non-throwing and simple for profile KV.
// Implemented by the save_load_system task (exports in ledger).
// If defaults differ, the linker will resolve the exact signatures.
[[nodiscard]] nlohmann::json ptv_profile_get(World& w,
                                             const std::string& key,
                                             const nlohmann::json& def_value = nlohmann::json::object());
void ptv_profile_set(World& w, const std::string& key, const nlohmann::json& value);
[[nodiscard]] std::string ptv_profile_current(World& w);
void ptv_save_now(World& w);

// Light-weight mastery state kept/persisted as JSON. We do not define custom
// ECS components to avoid cross-task header coupling; other systems can read
// the JSON via save_load_system using the same keys.
struct MasteryState {
    // Total unspent mastery points available to spend at kiosks
    int points{0};
    // Per-category unlock levels (0..3 typical). Keys: Attack/Defense/Control/Traversal/Utility/Heavy
    std::unordered_map<std::string, int> category_levels; 
    // Individual upgrade flags (e.g., "Defense.shield_hp+1")
    std::unordered_set<std::string> upgrades;

    static MasteryState from_json(const nlohmann::json& j) {
        MasteryState s;
        if (!j.is_object()) return s;
        s.points = j.value("points", 0);
        if (j.contains("category_levels") && j["category_levels"].is_object()) {
            for (auto it = j["category_levels"].begin(); it != j["category_levels"].end(); ++it) {
                if (it.value().is_number_integer()) s.category_levels[it.key()] = it.value().get<int>();
            }
        }
        if (j.contains("upgrades") && j["upgrades"].is_array()) {
            for (auto& u : j["upgrades"]) if (u.is_string()) s.upgrades.insert(u.get<std::string>());
        }
        return s;
    }
    nlohmann::json to_json() const {
        nlohmann::json j = nlohmann::json::object();
        j["points"] = points;
        nlohmann::json lev = nlohmann::json::object();
        for (auto& kv : category_levels) lev[kv.first] = kv.second;
        j["category_levels"] = std::move(lev);
        j["upgrades"] = nlohmann::json::array();
        for (auto& u : upgrades) j["upgrades"].push_back(u);
        return j;
    }
};

// Compute tuning from mastery. This produces the blob other systems can consume.
inline nlohmann::json compute_construct_tuning_json(const MasteryState& s) {
    auto get_level = [&](const char* k) -> int {
        auto it = s.category_levels.find(k);
        return it == s.category_levels.end() ? 0 : std::max(0, it->second);
    };
    // Base multipliers 1.0, reduce cost by 5% per level, clamp at 0.6
    auto cost_scale_for = [&](int lvl) {
        float m = 1.0f - 0.05f * static_cast<float>(lvl);
        return std::max(0.6f, m);
    };
    // Sustained drain reduction slightly softer: 4% per level, clamp 0.65
    auto drain_scale_for = [&](int lvl) {
        float m = 1.0f - 0.04f * static_cast<float>(lvl);
        return std::max(0.65f, m);
    };

    nlohmann::json cost = nlohmann::json::object();
    nlohmann::json drain = nlohmann::json::object();

    const char* cats[6] = {"Attack","Defense","Control","Traversal","Utility","Heavy"};
    for (auto* c : cats) {
        int lvl = get_level(c);
        cost[c] = cost_scale_for(lvl);
        // Drain mainly applicable to sustained constructs; apply for Defense/Control/Heavy
        if (std::string(c) == "Defense" || std::string(c) == "Control" || std::string(c) == "Heavy")
            drain[c] = drain_scale_for(lvl);
    }

    // Stability bonus: favors Defense and Control unlocks
    float stability = 0.0f;
    stability += 0.10f * static_cast<float>(get_level("Defense"));
    stability += 0.06f * static_cast<float>(get_level("Control"));
    if (s.upgrades.count("Defense.shield_hp+1")) stability += 0.05f;
    if (s.upgrades.count("Control.longer_tethers")) stability += 0.05f;
    stability = std::min(0.5f, stability); // cap at +50%

    // Simple player power proxy to feed AI difficulty tuning (0..)
    int total_levels = 0; for (auto& kv : s.category_levels) total_levels += std::max(0, kv.second);
    float player_power = static_cast<float>(total_levels) * 1.0f + static_cast<float>(s.upgrades.size()) * 0.5f;

    nlohmann::json j = nlohmann::json::object();
    j["cost_multipliers"] = std::move(cost);
    j["sustained_drain_multipliers"] = std::move(drain);
    j["stability_bonus"] = stability;
    j["player_power"] = player_power;
    return j;
}

// Merge a purchase request into mastery state if affordable.
inline bool apply_purchase(MasteryState& s, const nlohmann::json& purchase) {
    // Accepted shapes:
    //  { "category": "Attack", "level": 1, "cost": 2 }
    //  { "upgrade_id": "Defense.shield_hp+1", "cost": 3 }
    if (!purchase.is_object()) return false;
    int cost = purchase.value("cost", 0);
    if (cost <= 0 || s.points < cost) return false;

    if (purchase.contains("category")) {
        std::string cat = purchase.value("category", "");
        int lvl = purchase.value("level", 0);
        if (!cat.empty() && lvl > 0) {
            int& cur = s.category_levels[cat];
            if (lvl > cur) { cur = lvl; s.points -= cost; return true; }
            return false;
        }
    }
    if (purchase.contains("upgrade_id")) {
        std::string id = purchase.value("upgrade_id", "");
        if (!id.empty() && !s.upgrades.count(id)) { s.upgrades.insert(id); s.points -= cost; return true; }
    }
    return false;
}

// Parse kiosk command JSON from CMechanicBinding::Entry::params_json
inline void process_kiosk_commands(ptv::World& w, std::uint32_t entity, const nlohmann::json& params,
                                   MasteryState& s, bool& dirty_profile, bool& dirty_tuning) {
    (void)w; (void)entity;
    // Commands (all optional):
    //  "earn": int                     // add mastery points
    //  "purchase": { ... }             // see apply_purchase
    //  "batch_purchases": [ {..}, .. ] // apply list in order
    if (!params.is_object()) return;
    if (params.contains("earn") && params["earn"].is_number_integer()) {
        int add = params["earn"].get<int>();
        if (add > 0) { s.points += add; dirty_profile = dirty_tuning = true; }
    }
    if (params.contains("purchase")) {
        if (apply_purchase(s, params["purchase"])) { dirty_profile = dirty_tuning = true; }
    }
    if (params.contains("batch_purchases") && params["batch_purchases"].is_array()) {
        for (auto& p : params["batch_purchases"]) {
            if (apply_purchase(s, p)) { dirty_profile = dirty_tuning = true; }
        }
    }
}

// Read the live params_json for this mechanic binding on an entity.
inline nlohmann::json read_binding_params(World& w, std::uint32_t e, const std::string& mech_id) {
    if (!w.has<ptv::CMechanicBinding>(e)) return nlohmann::json::object();
    auto& mb = w.get<ptv::CMechanicBinding>(e);
    for (auto& ent : mb.entries) {
        if (ent.id == mech_id || ent.builtin_id == mech_id) {
            nlohmann::json j = nlohmann::json::parse(ent.params_json, nullptr, false);
            if (j.is_discarded()) return nlohmann::json::object();
            return j;
        }
    }
    return nlohmann::json::object();
}

// Profile keys
static constexpr const char* kProfileKey = "mastery_profile";          // persisted player mastery
static constexpr const char* kTuningKey  = "construct_tuning";         // published construct tuning
static constexpr const char* kPowerKey   = "player_power_hint";        // difficulty scaling hint

} // namespace ptv
