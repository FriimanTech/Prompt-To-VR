#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <nlohmann/json.hpp>

namespace ptv {

// Lightweight progression data container kept per-attached entity and also serialized into CTag.
struct ProgressionData {
    // Totals
    float xp_total{0.0f};
    float xp_unspent{0.0f};

    // Unlocked construct ids and families
    std::unordered_set<std::string> unlocked_items;       // e.g., "energy_bolt", "dome_shield"
    std::unordered_set<std::string> unlocked_families;    // e.g., "Attack", "Defense"

    // Tuning multipliers per-construct (applied multiplicatively in core by reading tags we place)
    // Keys: drain_mult.*, cooldown_mult.*, durability_mult.*
    std::unordered_map<std::string, float> drain_mult;       // id -> multiplier
    std::unordered_map<std::string, float> cooldown_mult;    // id -> multiplier
    std::unordered_map<std::string, float> durability_mult;  // id -> multiplier

    // Pinned quick-access list (mirrors to CTag on right-hand entity as pinned.<id>)
    std::vector<std::string> pinned;

    // Unlock tree raw for reference (costs, prereqs, effects)
    nlohmann::json unlock_tree;     // object: node -> { cost, prereq:[...], effects:{...} }

    // Soft cap rules (e.g., {"max_pinned":4, "tuning_cap":0.5, "diminish_after":2, "diminish_k":0.15})
    nlohmann::json soft_caps;       

    // XP curve optional (levels array or piecewise). We only need it to compute progress percentage for displays.
    nlohmann::json xp_curve;        
};

// Helpers -------------------------------------------------------------

inline float read_number_or(const nlohmann::json& j, const char* key, float defv) {
    if (j.is_object() && j.contains(key) && j.at(key).is_number()) return j.at(key).get<float>();
    return defv;
}

inline std::string read_string_or(const nlohmann::json& j, const char* key, const std::string& defv) {
    if (j.is_object() && j.contains(key) && j.at(key).is_string()) return j.at(key).get<std::string>();
    return defv;
}

inline std::vector<std::string> read_string_array(const nlohmann::json& j, const char* key) {
    std::vector<std::string> out;
    if (j.is_object() && j.contains(key) && j.at(key).is_array()) {
        for (auto& v : j.at(key)) if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

// Compute diminishing multiplier clamped to [cap, 1.0]. count_over is how many upgrades already stacked.
inline float apply_diminishing(float base_mult, int count_over, float diminish_k, float cap) {
    // base_mult is the target after one upgrade (e.g., 0.9). We move towards 1.0 as stacks grow using 1 - (1-base)/ (1 + k*n).
    const float delta = 1.0f - base_mult;
    const float adjusted = 1.0f - (delta / (1.0f + diminish_k * static_cast<float>(count_over)));
    return adjusted < cap ? cap : adjusted;
}

// Serialize to compact JSON for persistence in CTag.
inline std::string progression_to_json_string(const ProgressionData& p) {
    nlohmann::json j;
    j["xp_total"] = p.xp_total;
    j["xp_unspent"] = p.xp_unspent;
    j["unlocked_items"] = std::vector<std::string>(p.unlocked_items.begin(), p.unlocked_items.end());
    j["unlocked_families"] = std::vector<std::string>(p.unlocked_families.begin(), p.unlocked_families.end());
    j["pinned"] = p.pinned;
    j["drain_mult"] = p.drain_mult;
    j["cooldown_mult"] = p.cooldown_mult;
    j["durability_mult"] = p.durability_mult;
    // We keep the configuration snapshots minimal to reduce tag size
    if (!p.xp_curve.is_null()) j["xp_curve"] = p.xp_curve;
    if (!p.unlock_tree.is_null()) j["unlock_tree_digest"] = (std::size_t)p.unlock_tree.dump().size();
    if (!p.soft_caps.is_null()) j["soft_caps"] = p.soft_caps;
    return j.dump();
}

inline void progression_from_json_string(const std::string& s, ProgressionData& out) {
    auto j = nlohmann::json::parse(s, nullptr, false);
    if (!j.is_object()) return;
    out.xp_total = j.value("xp_total", 0.0f);
    out.xp_unspent = j.value("xp_unspent", 0.0f);
    if (j.contains("unlocked_items") && j["unlocked_items"].is_array()) {
        out.unlocked_items.clear();
        for (auto& v : j["unlocked_items"]) if (v.is_string()) out.unlocked_items.insert(v.get<std::string>());
    }
    if (j.contains("unlocked_families") && j["unlocked_families"].is_array()) {
        out.unlocked_families.clear();
        for (auto& v : j["unlocked_families"]) if (v.is_string()) out.unlocked_families.insert(v.get<std::string>());
    }
    if (j.contains("pinned") && j["pinned"].is_array()) {
        out.pinned.clear();
        for (auto& v : j["pinned"]) if (v.is_string()) out.pinned.push_back(v.get<std::string>());
    }
    if (j.contains("drain_mult") && j["drain_mult"].is_object()) out.drain_mult = j["drain_mult"].get<std::unordered_map<std::string,float>>();
    if (j.contains("cooldown_mult") && j["cooldown_mult"].is_object()) out.cooldown_mult = j["cooldown_mult"].get<std::unordered_map<std::string,float>>();
    if (j.contains("durability_mult") && j["durability_mult"].is_object()) out.durability_mult = j["durability_mult"].get<std::unordered_map<std::string,float>>();
    if (j.contains("soft_caps")) out.soft_caps = j["soft_caps"];
}

} // namespace ptv
