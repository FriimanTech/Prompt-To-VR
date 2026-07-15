#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "progression_mastery_system_shared.h"

namespace ptv {

class ProgressionMasterySystem : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        (void)e;
        // Load mastery profile
        auto j = ptv_profile_get(w, kProfileKey, nlohmann::json::object());
        state_ = MasteryState::from_json(j);
        // Ensure all categories are present with level 0
        const char* cats[6] = {"Attack","Defense","Control","Traversal","Utility","Heavy"};
        for (auto* c : cats) if (!state_.category_levels.count(c)) state_.category_levels[c] = 0;

        // Optionally grant starting points from params (one-time bootstrap for tests)
        int start_pts = params.value("starting_points", 0);
        if (start_pts > 0) state_.points += start_pts;

        // Publish current tuning immediately (applied on scene load or kiosk entry)
        publish_tuning_if_needed(w, /*force=*/true);
        persist_if_dirty(w, /*force=*/true);
    }

    void on_update(World& w, std::uint32_t e, float /*dt*/) override {
        // Read live commands from this binding (kiosk console)
        auto params = read_binding_params(w, e, id());
        bool dirty_profile = false, dirty_tuning = false;
        process_kiosk_commands(w, e, params, state_, dirty_profile, dirty_tuning);

        // Optional simple shorthand: { "unlock": {"Attack":1, "Defense":1}, "cost_each": 1 }
        if (params.contains("unlock") && params["unlock"].is_object()) {
            int c = params.value("cost_each", 1);
            for (auto it = params["unlock"].begin(); it != params["unlock"].end(); ++it) {
                if (it.value().is_number_integer()) {
                    nlohmann::json p = nlohmann::json::object();
                    p["category"] = it.key();
                    p["level"] = std::max(0, it.value().get<int>());
                    p["cost"] = c;
                    if (apply_purchase(state_, p)) { dirty_profile = dirty_tuning = true; }
                }
            }
        }

        if (dirty_tuning) publish_tuning_if_needed(w, /*force=*/true);
        if (dirty_profile) persist_if_dirty(w, /*force=*/true);
    }

    void on_detach(World& w, std::uint32_t /*e*/) override {
        // Final save-on-exit to satisfy persistence acceptance hook
        persist_if_dirty(w, /*force=*/true);
    }

    std::string id() const override { return "generated.progression_mastery_system"; }

private:
    MasteryState state_{};

    void publish_tuning_if_needed(World& w, bool force) {
        static nlohmann::json last;
        nlohmann::json cur = compute_construct_tuning_json(state_);
        if (force || cur.dump() != last.dump()) {
            ptv_profile_set(w, kTuningKey, cur);
            // Also surface a simplified power hint for enemy AI
            if (cur.contains("player_power")) {
                nlohmann::json pw = nlohmann::json::object();
                pw["value"] = cur["player_power"];
                ptv_profile_set(w, kPowerKey, pw);
            }
            last = std::move(cur);
        }
    }

    void persist_if_dirty(World& w, bool force) {
        static std::string last_blob;
        auto blob = state_.to_json().dump();
        if (force || blob != last_blob) {
            ptv_profile_set(w, kProfileKey, state_.to_json());
            ptv_save_now(w); // ensure persistence across sessions (acceptance hook)
            last_blob = std::move(blob);
        }
    }
};

PTV_REGISTER_MECHANIC("generated.progression_mastery_system", ProgressionMasterySystem)

} // namespace ptv
