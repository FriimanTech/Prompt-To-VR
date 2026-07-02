#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "progression_shared.h"

namespace ptv {

// forward decls from sibling TUs
void progression_recompute_and_publish(World& w, std::uint32_t e,
                                       const ProgTree& tree,
                                       const std::unordered_map<std::string, SoftCapRule>& caps,
                                       ProgressionState& st);
bool progression_try_spend(World& w, std::uint32_t e,
                           const ProgTree& tree,
                           const std::unordered_map<std::string, SoftCapRule>& caps,
                           ProgressionState& st,
                           const std::string& node_id,
                           std::string* failure_reason_out);
void progression_publish_pins(World& w, std::uint32_t e, const std::vector<std::string>& pins);

// forward decls from persistence TU
void progression_publish_save_section(World& w, std::uint32_t e, const ProgressionState& s);
void progression_request_save(World& w, std::uint32_t e);
void progression_deserialize_into(ProgressionState& s, const nlohmann::json& j);

// helpers from integration TU
std::optional<std::string> parse_spend_request(const std::string& tag);
std::optional<std::pair<std::string,int>> parse_pin_request(const std::string& tag);
int parse_xp_if_any(const std::string& tag, std::string* event_id_out);
bool is_save_collect_start(const std::string& tag);
bool is_load_section_for_us(const std::string& tag);

class ProgressionAndUnlocks final : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        // Parse config
        if (params.contains("unlock_tree")) tree_ = parse_unlock_tree(params.at("unlock_tree"));
        soft_caps_ = parse_soft_caps(params.value("soft_cap_rules", nlohmann::json::object()));
        max_pins_ = std::max(1, params.value("max_pins", 6));

        // UI page registration via HUD/Wrist contract
        {
            nlohmann::json page{{"id","progression"},{"title","Progression"},{"order", 30}};
            std::string s = std::string("ui.page.register:") + page.dump();
            if (w.has<CTag>(e)) { auto& v = w.get<CTag>(e).tags; v.push_back(s);} else { CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t)); }
        }
        // Register our save section with save_persistence (optional but informative)
        {
            nlohmann::json reg{{"section_id","progression"}};
            std::string s = std::string("save.register:") + reg.dump();
            if (w.has<CTag>(e)) { auto& v = w.get<CTag>(e).tags; v.push_back(s);} else { CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t)); }
        }
        // Small toast
        {
            nlohmann::json toast{{"text","Progression ready"},{"ttl",1.2}};
            std::string s = std::string("ui.toast:") + toast.dump();
            if (w.has<CTag>(e)) { auto& v = w.get<CTag>(e).tags; v.push_back(s);} else { CTag t; t.tags.push_back(s); w.emplace_or_replace<CTag>(e, std::move(t)); }
        }
    }

    void on_update(World& w, std::uint32_t e, float /*dt*/) override {
        // Defensive: return early if entity died
        if (!w.valid(e)) return;

        // Scan world tags for inputs and system events
        auto& reg = w.registry();
        reg.view<CTag>().each([&](entt::entity ent, CTag& tag){
            (void)ent; // not needed
            // Iterate a snapshot copy to avoid iterator invalidation if someone else edits tags
            auto tags = tag.tags;
            for (const auto& s : tags) {
                // XP accumulation from mission/objective events
                std::string evt_id;
                int xp = parse_xp_if_any(s, &evt_id);
                if (xp > 0) {
                    if (!evt_id.empty() && state_.processed_event_ids.count(evt_id)) {
                        // already processed
                    } else {
                        state_.total_xp_earned += xp;
                        state_.unspent_xp += xp;
                        if (!evt_id.empty()) state_.processed_event_ids.insert(evt_id);
                        // brief toast
                        nlohmann::json toast{{"text", std::string("+XP ")+std::to_string(xp)},{"ttl",1.0}};
                        std::string ts = std::string("ui.toast:") + toast.dump();
                        if (w.has<CTag>(e)) { auto& v = w.get<CTag>(e).tags; v.push_back(ts);} else { CTag t; t.tags.push_back(ts); w.emplace_or_replace<CTag>(e, std::move(t)); }
                    }
                }

                // Spend/unlock requests
                if (auto node = parse_spend_request(s)) {
                    std::string reason;
                    if (!progression_try_spend(w, e, tree_, soft_caps_, state_, *node, &reason)) {
                        nlohmann::json toast{{"text", std::string("Unlock failed: ")+reason},{"ttl",1.5}};
                        std::string ts = std::string("ui.toast:") + toast.dump();
                        if (w.has<CTag>(e)) { auto& v = w.get<CTag>(e).tags; v.push_back(ts);} else { CTag t; t.tags.push_back(ts); w.emplace_or_replace<CTag>(e, std::move(t)); }
                    } else {
                        // Persist on success
                        progression_publish_save_section(w, e, state_);
                        progression_request_save(w, e);
                    }
                }

                // Pin requests
                if (auto pin = parse_pin_request(s)) {
                    push_unique(state_.pinned, pin->first, static_cast<std::size_t>(pin->second));
                    progression_publish_pins(w, e, state_.pinned);
                    progression_publish_save_section(w, e, state_);
                    progression_request_save(w, e);
                }

                // Persistence integration
                if (is_save_collect_start(s)) {
                    progression_publish_save_section(w, e, state_);
                }
                if (is_load_section_for_us(s)) {
                    // payload delivered separately via load.completed or load.section itself
                    constexpr std::string_view pfx = "load.section:";
                    auto j = nlohmann::json::parse(s.substr(pfx.size()), nullptr, false);
                    if (j.is_object() && j.contains("payload") && j.at("payload").is_object()) {
                        progression_deserialize_into(state_, j.at("payload"));
                        // After load, recompute and publish enable/tuning to sync other systems
                        progression_recompute_and_publish(w, e, tree_, soft_caps_, state_);
                        progression_publish_pins(w, e, state_.pinned);
                    }
                }
            }
        });

        // Optional: periodically re-publish pins to keep selection wheel in sync (cheap, idempotent)
        republish_timer_ += w.delta_time();
        if (republish_timer_ > 2.0f) {
            republish_timer_ = 0.0f;
            progression_publish_pins(w, e, state_.pinned);
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.progression_and_unlocks"; }

private:
    ProgTree tree_{};
    std::unordered_map<std::string, SoftCapRule> soft_caps_{};
    ProgressionState state_{};
    int max_pins_{6};
    float republish_timer_{0.0f};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.progression_and_unlocks", ptv::ProgressionAndUnlocks)
