#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <algorithm>

// [ptv-bounds] removed disallowed include: progression_shared.h

namespace ptv {

// Small utilities to read/write key=value style tags inside CTag.tags
namespace tagkv {
    inline std::optional<std::string> get_value(const CTag& tag, const std::string& key) {
        const std::string prefix = key + "=";
        for (const auto& t : tag.tags) if (t.rfind(prefix, 0) == 0) return t.substr(prefix.size());
        return std::nullopt;
    }
    inline void set_value(CTag& tag, const std::string& key, const std::string& value) {
        const std::string prefix = key + "=";
        for (auto& t : tag.tags) {
            if (t.rfind(prefix, 0) == 0) { t = prefix + value; return; }
        }
        tag.tags.push_back(prefix + value);
    }
    inline void set_flag(CTag& tag, const std::string& key) {
        for (auto& t : tag.tags) if (t == key) return; // already present
        tag.tags.push_back(key);
    }
    inline void remove_flag(CTag& tag, const std::string& key) {
        auto& v = tag.tags; v.erase(std::remove(v.begin(), v.end(), key), v.end());
    }
}

// Integration helpers to find player right-hand entity to receive unlock tags
static entt::entity find_right_hand_entity(World& w) {
    // 1) Common explicit ids in scenes
    const char* ids[] = {"player_right_hand", "player_hand_r", "player_right_hand_avatar", "player_ring_right_hand", "Right Hand", "player_hand_right"};
    for (auto* s : ids) {
        auto e = w.find(s);
        if (e != entt::null) return e;
    }
    // 2) Fallback: search for entity with CTag having both player_avatar and (right_hand|hand,right)
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ent : view) {
        const auto& t = view.get<CTag>(ent);
        bool is_player = std::find(t.tags.begin(), t.tags.end(), std::string("player_avatar")) != t.tags.end();
        bool rightish = (std::find(t.tags.begin(), t.tags.end(), std::string("right_hand")) != t.tags.end()) ||
                        (std::find(t.tags.begin(), t.tags.end(), std::string("hand")) != t.tags.end());
        if (is_player && rightish) return ent;
    }
    return entt::null;
}

// Push unlock and tuning tags to the right hand so ring_construct_core can read them.
static void mirror_to_right_hand(World& w, const ProgressionData& p) {
    auto rhs = find_right_hand_entity(w);
    if (rhs == entt::null) return;
    // Ensure a CTag exists
    w.emplace_or_replace<CTag>(static_cast<std::uint32_t>(rhs), CTag{});
    auto& tag = w.get<CTag>(static_cast<std::uint32_t>(rhs));

    // Families and individual unlocks
    for (const auto& fam : p.unlocked_families) tagkv::set_flag(tag, std::string("unlocked_family.") + fam);
    for (const auto& it : p.unlocked_items) tagkv::set_flag(tag, std::string("unlocked.") + it);

    // Pinned
    // First clear previous pins we own (best-effort small sweep)
    {
        auto& vec = tag.tags;
        vec.erase(std::remove_if(vec.begin(), vec.end(), [](const std::string& s){ return s.rfind("pinned.", 0) == 0; }), vec.end());
    }
    for (const auto& id : p.pinned) tagkv::set_flag(tag, std::string("pinned.") + id);

    // Tuning multipliers -> key=value
    for (const auto& kv : p.drain_mult) tagkv::set_value(tag, std::string("tuning.drain_mult.") + kv.first, std::to_string(kv.second));
    for (const auto& kv : p.cooldown_mult) tagkv::set_value(tag, std::string("tuning.cooldown_mult.") + kv.first, std::to_string(kv.second));
    for (const auto& kv : p.durability_mult) tagkv::set_value(tag, std::string("tuning.durability_mult.") + kv.first, std::to_string(kv.second));
}

// Persist into a tag on the player body (or this entity if no body found)
static void persist_to_save_tag(World& w, std::uint32_t fallback_entity, const ProgressionData& p) {
    // Prefer a player body entity id if available
    entt::entity target = w.find("player_body");
    if (target == entt::null) target = w.find("player_body_stub");
    if (target == entt::null) target = static_cast<entt::entity>(fallback_entity);

    w.emplace_or_replace<CTag>(static_cast<std::uint32_t>(target), CTag{});
    auto& tag = w.get<CTag>(static_cast<std::uint32_t>(target));
    tagkv::set_value(tag, "save.progression_and_unlocks", progression_to_json_string(p));
}

static bool try_load_from_save_tag(World& w, ProgressionData& out) {
    entt::entity e = w.find("player_body");
    if (e == entt::null) e = w.find("player_body_stub");
    if (e == entt::null) return false;
    if (!w.has<CTag>(static_cast<std::uint32_t>(e))) return false;
    auto& tag = w.get<CTag>(static_cast<std::uint32_t>(e));
    auto v = tagkv::get_value(tag, "save.progression_and_unlocks");
    if (!v.has_value()) return false;
    progression_from_json_string(*v, out);
    return true;
}

class ProgressionAndUnlocks final : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        // Seed config
        data_.xp_curve = params.contains("xp_curve") ? params.at("xp_curve") : nlohmann::json{};
        data_.unlock_tree = params.contains("unlock_tree") ? params.at("unlock_tree") : nlohmann::json{};
        data_.soft_caps = params.contains("soft_cap_rules") ? params.at("soft_cap_rules") : nlohmann::json{};

        // Attempt to restore persisted state
        (void)try_load_from_save_tag(w, data_);

        // Optional immediate XP grant at attach (useful for tests)
        float boot_xp = params.value("grant_xp_on_attach", 0.0f);
        grant_xp(boot_xp);

        // Mirror to right hand so wheel can reflect any restored unlocks
        mirror_to_right_hand(w, data_);
        // Persist a fresh snapshot immediately so save tests find a section
        persist_to_save_tag(w, e, data_);
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        // XP accumulation heuristic: if a mission is in progress, accrue a trickle defined by params on binding
        // We do not have direct mission API headers here; the test harness can drive XP via tags on this entity
        float xp_tick = pending_grant_from_binding(w, e);
        if (xp_tick > 0.0f) grant_xp(xp_tick);

        // Process queued unlock or pin requests from binding params_json of this entity (driven by UI/terminal)
        process_binding_requests(w, e);

        // Periodically mirror/persist (cheap per-frame for now; could be throttled)
        mirror_to_right_hand(w, data_);
        persist_to_save_tag(w, e, data_);
    }

    void on_detach(World& w, std::uint32_t e) override {
        // Final persist on detach
        persist_to_save_tag(w, e, data_);
    }

    std::string id() const override { return "generated.progression_and_unlocks"; }

private:
    ProgressionData data_{};

    void grant_xp(float v) {
        if (v <= 0.0f) return;
        data_.xp_total += v;
        data_.xp_unspent += v;
    }

    // Read a one-shot xp grant from our CMechanicBinding entry params_json: {"grant_xp": <float>}
    float pending_grant_from_binding(World& w, std::uint32_t e) {
        if (!w.has<CMechanicBinding>(e)) return 0.0f;
        auto& mb = w.get<CMechanicBinding>(e);
        float grant{0.0f};
        for (auto& entry : mb.entries) {
            if (entry.id == id() || entry.builtin_id == id()) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object() && j.contains("grant_xp") && j["grant_xp"].is_number()) {
                    grant += j["grant_xp"].get<float>();
                    // Note: we cannot mutate params_json here; assume driver clears it next frame.
                }
            }
        }
        return grant;
    }

    void process_binding_requests(World& w, std::uint32_t e) {
        if (!w.has<CMechanicBinding>(e)) return;
        auto& mb = w.get<CMechanicBinding>(e);
        for (auto& entry : mb.entries) {
            if (entry.id != id() && entry.builtin_id != id()) continue;
            auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
            if (!j.is_object()) continue;

            // Unlock request: { unlock: { id:"energy_bolt" } } or family unlock { family:"Attack" }
            if (j.contains("unlock")) {
                const auto& uj = j["unlock"];
                if (uj.is_object()) handle_unlock_request(uj);
            }
            // Pin/unpin: { pin: "energy_bolt" } / { unpin:"energy_bolt" }
            if (j.contains("pin") && j["pin"].is_string()) pin_item(j["pin"].get<std::string>());
            if (j.contains("unpin") && j["unpin"].is_string()) unpin_item(j["unpin"].get<std::string>());
        }
    }

    void handle_unlock_request(const nlohmann::json& uj) {
        // Determine target id or family
        const bool is_family = uj.contains("family") && uj["family"].is_string();
        const bool is_item = uj.contains("id") && uj["id"].is_string();
        float cost = read_number_or(uj, "cost", 0.0f);
        // Optionally override cost by reading from unlock_tree if present
        if (is_item && data_.unlock_tree.is_object() && data_.unlock_tree.contains(uj.at("id").get<std::string>())) {
            auto& node = data_.unlock_tree.at(uj.at("id").get<std::string>());
            cost = read_number_or(node, "cost", cost);
        }
        if (is_family && data_.unlock_tree.is_object() && data_.unlock_tree.contains(uj.at("family").get<std::string>())) {
            auto& node = data_.unlock_tree.at(uj.at("family").get<std::string>());
            cost = read_number_or(node, "cost", cost);
        }
        if (cost <= 0.0f || data_.xp_unspent < cost) return; // insufficient

        // Prereq check (simple: all prereq strings must be unlocked either item or family)
        auto require_ok = [&](const nlohmann::json& node)->bool{
            auto reqs = read_string_array(node, "prereq");
            for (const auto& r : reqs) {
                if (!data_.unlocked_items.count(r) && !data_.unlocked_families.count(r)) return false;
            }
            return true;
        };

        if (is_item) {
            const std::string id = uj.at("id").get<std::string>();
            const nlohmann::json node = (data_.unlock_tree.is_object() && data_.unlock_tree.contains(id)) ? data_.unlock_tree.at(id) : nlohmann::json{};
            if (!require_ok(node)) return;
            if (!data_.unlocked_items.count(id)) {
                data_.unlocked_items.insert(id);
                data_.xp_unspent -= cost;
                apply_effects_from_node(id, node);
            }
        } else if (is_family) {
            const std::string fam = uj.at("family").get<std::string>();
            const nlohmann::json node = (data_.unlock_tree.is_object() && data_.unlock_tree.contains(fam)) ? data_.unlock_tree.at(fam) : nlohmann::json{};
            if (!require_ok(node)) return;
            if (!data_.unlocked_families.count(fam)) {
                data_.unlocked_families.insert(fam);
                data_.xp_unspent -= cost;
                apply_effects_from_node(fam, node);
            }
        }
    }

    void apply_effects_from_node(const std::string& key, const nlohmann::json& node) {
        // Effects example shape:
        // {
        //   effects: {
        //     drain_mult: { "dome_shield": 0.9, "continuous_beam": 0.95 },
        //     cooldown_mult: { "energy_bolt": 0.9 },
        //     durability_mult: { "barrier_wall": 1.1 }
        //   }
        // }
        if (!node.is_object() || !node.contains("effects")) return;
        const auto& ef = node.at("effects");

        auto apply_cat = [&](const char* cat, std::unordered_map<std::string,float>& dst){
            if (!ef.contains(cat) || !ef.at(cat).is_object()) return;
            for (auto it = ef.at(cat).begin(); it != ef.at(cat).end(); ++it) {
                if (!it.value().is_number()) continue;
                float base = it.value().get<float>();
                int count_over = 0;
                if (dst.count(it.key())) {
                    // We treat stacking as additional diminishing steps
                    count_over = 1; 
                }
                const float k = read_number_or(data_.soft_caps, "diminish_k", 0.15f);
                const float cap = read_number_or(data_.soft_caps, "tuning_cap", (std::string(cat)=="durability_mult"? 0.5f:0.6f));
                dst[it.key()] = apply_diminishing(base, count_over, k, cap);
            }
        };

        apply_cat("drain_mult", data_.drain_mult);
        apply_cat("cooldown_mult", data_.cooldown_mult);
        apply_cat("durability_mult", data_.durability_mult);
    }

    void pin_item(const std::string& id) {
        const int max_pins = static_cast<int>(read_number_or(data_.soft_caps, "max_pinned", 4.0f));
        if (std::find(data_.pinned.begin(), data_.pinned.end(), id) != data_.pinned.end()) return;
        if ((int)data_.pinned.size() >= max_pins) {
            // Soft behavior: evict oldest
            data_.pinned.erase(data_.pinned.begin());
        }
        data_.pinned.push_back(id);
    }

    void unpin_item(const std::string& id) {
        auto& v = data_.pinned;
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.progression_and_unlocks", ptv::ProgressionAndUnlocks)
