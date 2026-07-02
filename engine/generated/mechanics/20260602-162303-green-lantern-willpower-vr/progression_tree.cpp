#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#include "progression_shared.h"

namespace ptv {

static bool has_all_prereq(const ProgressionState& s, const ProgNode& n) {
    for (const auto& p : n.prereq) if (!s.unlocked_nodes.count(p)) return false;
    return true;
}

static void aggregate_family_effects(const ProgTree& tree,
                                     const ProgressionState& s,
                                     const std::unordered_map<std::string, SoftCapRule>& caps,
                                     std::unordered_map<std::string, ProgressionState::FamilyTuning>& out) {
    out.clear();
    // Start with identity effects per family for any unlocked nodes
    for (const auto& node_id : s.unlocked_nodes) {
        auto it = tree.nodes.find(node_id);
        if (it == tree.nodes.end()) continue;
        const ProgNode& n = it->second;
        auto& fam = out[n.family];
        fam.eff = combine(fam.eff, n.effects);
    }
    // Apply caps
    for (auto& kv : out) {
        kv.second.eff = apply_soft_caps_effect(kv.second.eff, caps);
    }
}

// Emit enable + tuning tags for a family when first unlocked or when tuning changes
static void publish_construct_enable(World& w, std::uint32_t e, const std::string& family) {
    std::string s = std::string("construct.enable:") + nlohmann::json({{"family", family}}).dump();
    if (w.has<CTag>(e)) {
        auto& tags = w.get<CTag>(e).tags; tags.push_back(s);
    } else {
        CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t));
    }
}

static void publish_construct_tuning(World& w, std::uint32_t e, const std::string& family, const ProgNodeEffect& eff) {
    nlohmann::json j{{"family", family}, {"drain_mult", eff.drain_mult}, {"cooldown_mult", eff.cooldown_mult}, {"durability_mult", eff.durability_mult}};
    std::string s = std::string("construct.tuning:") + j.dump();
    if (w.has<CTag>(e)) { auto& tags = w.get<CTag>(e).tags; tags.push_back(s);} else { CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t)); }
}

// Public helper for other compilation units in this task
void progression_recompute_and_publish(World& w, std::uint32_t e,
                                       const ProgTree& tree,
                                       const std::unordered_map<std::string, SoftCapRule>& caps,
                                       ProgressionState& st) {
    // Families enabled are those with at least one unlocked node that defines a family id.
    std::unordered_set<std::string> newly_enabled;

    // Re-aggregate tuning
    aggregate_family_effects(tree, st, caps, st.tuning_by_family);

    // Scan through nodes for any families to enable
    for (const auto& node_id : st.unlocked_nodes) {
        auto it = tree.nodes.find(node_id); if (it == tree.nodes.end()) continue;
        const std::string& fam = it->second.family;
        if (fam.empty()) continue;
        if (!st.enabled_families.count(fam)) {
            st.enabled_families.insert(fam);
            newly_enabled.insert(fam);
        }
    }

    // Publish events
    for (const auto& fam : newly_enabled) publish_construct_enable(w, e, fam);
    for (const auto& kv : st.tuning_by_family) publish_construct_tuning(w, e, kv.first, kv.second.eff);
}

bool progression_try_spend(World& w, std::uint32_t e,
                           const ProgTree& tree,
                           const std::unordered_map<std::string, SoftCapRule>& caps,
                           ProgressionState& st,
                           const std::string& node_id,
                           std::string* failure_reason_out) {
    auto it = tree.nodes.find(node_id);
    if (it == tree.nodes.end()) { if (failure_reason_out) *failure_reason_out = "unknown_node"; return false; }
    const ProgNode& n = it->second;
    if (st.unlocked_nodes.count(node_id)) { if (failure_reason_out) *failure_reason_out = "already_unlocked"; return false; }
    if (st.unspent_xp < n.cost_xp) { if (failure_reason_out) *failure_reason_out = "not_enough_xp"; return false; }
    if (!has_all_prereq(st, n)) { if (failure_reason_out) *failure_reason_out = "missing_prereq"; return false; }

    st.unspent_xp -= n.cost_xp;
    st.unlocked_nodes.insert(node_id);

    // Toast + re-aggregate + publish
    {
        nlohmann::json toast{{"text", std::string("Unlocked: ")+node_id}, {"ttl", 2.0}};
        std::string s = std::string("ui.toast:") + toast.dump();
        if (w.has<CTag>(e)) { auto& tags = w.get<CTag>(e).tags; tags.push_back(s);} else { CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t)); }
    }

    progression_recompute_and_publish(w, e, tree, caps, st);
    return true;
}

void progression_publish_pins(World& w, std::uint32_t e, const std::vector<std::string>& pins) {
    nlohmann::json j{{"pinned", pins}};
    std::string s = std::string("construct.pinned:") + j.dump();
    if (w.has<CTag>(e)) { auto& tags = w.get<CTag>(e).tags; tags.push_back(s);} else { CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t)); }
}

} // namespace ptv
