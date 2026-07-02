#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace ptv {

struct ProgNodeEffect {
    // Multipliers applied to construct systems; default 1.0 means no change
    float drain_mult{1.0f};
    float cooldown_mult{1.0f};
    float durability_mult{1.0f};
};

struct ProgNode {
    std::string id;                  // unique key (e.g., "attack_core", "defense_ii")
    std::string family;              // construct family or upgrade bucket this node targets
    int cost_xp{0};
    std::vector<std::string> prereq; // node ids
    ProgNodeEffect effects;          // multipliers this node contributes when unlocked
};

struct SoftCapRule {
    // For aggregate multipliers after combining all unlocked effects
    float min_val{0.5f};   // clamp low
    float max_val{2.0f};   // clamp high
    float k{0.0f};         // optional non-linear squeeze; 0 disables
};

struct ProgTree {
    // Indexed by node id
    std::unordered_map<std::string, ProgNode> nodes;
};

struct ProgressionState {
    // Currency
    int total_xp_earned{0};
    int unspent_xp{0};

    // Ownership
    std::unordered_set<std::string> unlocked_nodes;    // node ids
    std::unordered_set<std::string> enabled_families;  // family ids enabled via unlocks

    // Aggregated tuning per family after soft caps
    struct FamilyTuning { ProgNodeEffect eff; };
    std::unordered_map<std::string, FamilyTuning> tuning_by_family;

    // Loadout pins (construct ids)
    std::vector<std::string> pinned; // order matters; unique

    // Recent mission ids processed to avoid double-counting (best-effort)
    std::unordered_set<std::string> processed_event_ids;
};

// ------------ Parsing helpers -------------
inline ProgNodeEffect parse_effects(const nlohmann::json& j) {
    ProgNodeEffect e{};
    if (j.is_object()) {
        e.drain_mult = j.value("drain_mult", 1.0f);
        e.cooldown_mult = j.value("cooldown_mult", 1.0f);
        e.durability_mult = j.value("durability_mult", 1.0f);
    }
    return e;
}

inline ProgTree parse_unlock_tree(const nlohmann::json& j) {
    ProgTree t{};
    if (!j.is_object() || !j.contains("nodes") || !j.at("nodes").is_array()) return t;
    for (const auto& n : j.at("nodes")) {
        if (!n.is_object()) continue;
        ProgNode node{};
        node.id = n.value("id", std::string{});
        node.family = n.value("family", std::string{});
        node.cost_xp = n.value("cost_xp", 0);
        if (n.contains("prereq") && n.at("prereq").is_array()) {
            for (const auto& p : n.at("prereq")) node.prereq.push_back(p.get<std::string>());
        }
        if (n.contains("effects")) node.effects = parse_effects(n.at("effects"));
        if (!node.id.empty()) t.nodes[node.id] = std::move(node);
    }
    return t;
}

inline std::unordered_map<std::string, SoftCapRule> parse_soft_caps(const nlohmann::json& j) {
    std::unordered_map<std::string, SoftCapRule> m;
    if (!j.is_object()) return m;
    auto parse_one = [](const nlohmann::json& jj){
        SoftCapRule r; r.min_val = jj.value("min", 0.5f); r.max_val = jj.value("max", 2.0f); r.k = jj.value("k", 0.0f); return r; };
    if (j.contains("drain_mult")) m["drain_mult"] = parse_one(j.at("drain_mult"));
    if (j.contains("cooldown_mult")) m["cooldown_mult"] = parse_one(j.at("cooldown_mult"));
    if (j.contains("durability_mult")) m["durability_mult"] = parse_one(j.at("durability_mult"));
    return m;
}

// Combine two ProgNodeEffect multiplicatively
inline ProgNodeEffect combine(const ProgNodeEffect& a, const ProgNodeEffect& b) {
    ProgNodeEffect r{};
    r.drain_mult = a.drain_mult * b.drain_mult;
    r.cooldown_mult = a.cooldown_mult * b.cooldown_mult;
    r.durability_mult = a.durability_mult * b.durability_mult;
    return r;
}

inline float apply_soft_cap_scalar(float v, const SoftCapRule* rule) {
    if (!rule) return v;
    // Clamp first
    v = std::max(rule->min_val, std::min(rule->max_val, v));
    if (rule->k <= 0.0f) return v;
    // Smooth squeeze around 1.0 using a gentle non-linear mapping
    // y = 1 + (v-1) / (1 + k * |v-1|)
    float d = v - 1.0f; float ad = std::abs(d);
    float y = 1.0f + d / (1.0f + rule->k * ad);
    // Re-clamp to bounds
    y = std::max(rule->min_val, std::min(rule->max_val, y));
    return y;
}

inline ProgNodeEffect apply_soft_caps_effect(const ProgNodeEffect& e, const std::unordered_map<std::string, SoftCapRule>& rules) {
    ProgNodeEffect r = e;
    auto it = rules.find("drain_mult"); r.drain_mult = apply_soft_cap_scalar(r.drain_mult, it==rules.end()?nullptr:&it->second);
    it = rules.find("cooldown_mult"); r.cooldown_mult = apply_soft_cap_scalar(r.cooldown_mult, it==rules.end()?nullptr:&it->second);
    it = rules.find("durability_mult"); r.durability_mult = apply_soft_cap_scalar(r.durability_mult, it==rules.end()?nullptr:&it->second);
    return r;
}

// Utility: ensure unique push to vector
inline void push_unique(std::vector<std::string>& v, const std::string& s, std::size_t max_keep) {
    if (s.empty()) return;
    v.erase(std::remove(v.begin(), v.end(), s), v.end());
    v.insert(v.begin(), s);
    if (v.size() > max_keep) v.resize(max_keep);
}

} // namespace ptv
