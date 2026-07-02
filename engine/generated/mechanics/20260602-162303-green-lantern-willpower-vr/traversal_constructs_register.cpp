#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>

#include "traversal_constructs_shared.h"

namespace ptv {

class TraversalConstructsExtension : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        auto& st = state_[e];
        // Defaults from params (if provided in binding)
        st.ladder.spacing_m = params.value("step_spacing_m", 0.35f);
        st.ladder.count = params.value("step_count", 5);
        st.ladder.max_length_m = params.value("ladder_max_length_m", 3.0f);
        st.ram.speed_mps = params.value("ram_speed_mps", 4.0f);
        st.ram.stabilize = params.value("ram_stabilize", true);
        st.ram.cancel_on_collision = params.value("ram_cancel_on_collision", true);
        (void)w;
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        auto it = state_.find(e);
        if (it == state_.end()) return;
        auto& st = it->second;

        // Read per-frame inputs from CMechanicBinding on this entity (params_json is authoritative this frame)
        nlohmann::json p = last_params_from_bindings(w, e);
        st.last_params = p;

        // Determine mode selection
        std::string mode = p.value("construct", std::string("none")); // expected: "ladder" | "ram" | "none"
        bool ladder_preview = p.value("ladder_preview", false) || (mode == "ladder" && p.value("preview", true));
        bool ram_request = (mode == "ram") && (p.value("confirm", false) || p.value("right_trigger_pressed", false));
        bool confirm = p.value("confirm", false) || p.value("right_trigger_pressed", false);
        bool cancel = p.value("cancel", false) || p.value("b_pressed", false);

        // Allow live tuning for ladders during preview
        if (p.contains("step_spacing_m")) st.ladder.spacing_m = p.value("step_spacing_m", st.ladder.spacing_m);
        if (p.contains("step_count")) st.ladder.count = p.value("step_count", st.ladder.count);
        if (p.contains("ladder_max_length_m")) st.ladder.max_length_m = p.value("ladder_max_length_m", st.ladder.max_length_m);
        if (p.contains("ram_speed_mps")) st.ram.speed_mps = p.value("ram_speed_mps", st.ram.speed_mps);
        if (p.contains("ram_stabilize")) st.ram.stabilize = p.value("ram_stabilize", st.ram.stabilize);
        if (p.contains("ram_cancel_on_collision")) st.ram.cancel_on_collision = p.value("ram_cancel_on_collision", st.ram.cancel_on_collision);

        // Fetch hand transform for placement
        Vec3 hand_pos{0,0,0}; Quat hand_rot{1,0,0,0};
        if (w.has<CTransform>(e)) {
            auto& tr = w.get<CTransform>(e).t; hand_pos = tr.position; hand_rot = tr.rotation;
        }

        // Surface targeting data from core (if present)
        Vec3 surf_point = p.contains("surface_point") ? Vec3{ p["surface_point"].value("x", hand_pos.x), p["surface_point"].value("y", hand_pos.y), p["surface_point"].value("z", hand_pos.z) } : hand_pos;
        Vec3 surf_normal = p.contains("surface_normal") ? Vec3{ p["surface_normal"].value("x", 0.0f), p["surface_normal"].value("y", 0.0f), p["surface_normal"].value("z", 1.0f) } : quat_rotate(hand_rot, Vec3{0,0,1});

        // Cancel flow clears previews and rides
        if (cancel) {
            cleanup_spawned(w, st.ladder.steps);
            if (st.ram.ram != entt::null && w.registry().valid(st.ram.ram)) w.registry().destroy(st.ram.ram);
            st = TraversalState{}; // reset to defaults
            return;
        }

        // Ladders preview and confirm
        if (ladder_preview) {
            st.mode = TravMode::Ladder;
            ladders_preview_update(w, e, st, surf_point, surf_normal, hand_rot, dt);
            if (confirm) {
                ladders_confirm_finalize(w, st);
                // Feedback
                publish_haptics(w, static_cast<entt::entity>(e), "construct_form_heavy", "right", 0.6f, 0.08f);
            }
        }

        // Ram ride on confirm
        if (ram_request) {
            st.mode = TravMode::Ram;
            ram_update(w, e, st, hand_pos, hand_rot, dt, false);
        }

        // If already riding, continue motion; allow B to cancel anytime
        if (st.mode == TravMode::Ram && st.ram.riding) {
            bool ram_cancel = p.value("b_pressed", false) || p.value("cancel", false);
            ram_update(w, e, st, hand_pos, hand_rot, dt, ram_cancel);
        }

        // Lifetime mgmt for placed ladder steps
        for (auto& s : st.ladder.steps) {
            if (s.e != entt::null && w.registry().valid(s.e)) {
                if (st.ladder.confirmed) {
                    s.ttl_s -= dt;
                    if (s.ttl_s <= 0.0f) { w.registry().destroy(s.e); s.e = entt::null; }
                }
            }
        }
        if (st.ladder.confirmed) {
            // Cull nulls from vector periodically
            st.ladder.steps.erase(std::remove_if(st.ladder.steps.begin(), st.ladder.steps.end(), [&](const Spawned& sp){ return sp.e==entt::null; }), st.ladder.steps.end());
        }
    }

    void on_detach(World& w, std::uint32_t e) override {
        auto it = state_.find(e);
        if (it == state_.end()) return;
        // Cleanup any spawned entities
        cleanup_spawned(w, it->second.ladder.steps);
        if (it->second.ram.ram != entt::null && w.registry().valid(it->second.ram.ram)) w.registry().destroy(it->second.ram.ram);
        state_.erase(it);
    }

    std::string id() const override { return "generated.traversal_constructs_extension"; }

private:
    std::unordered_map<std::uint32_t, TraversalState> state_;

    static nlohmann::json parse_params_safe(const std::string& s) {
        return nlohmann::json::parse(s, nullptr, false);
    }

    nlohmann::json last_params_from_bindings(World& w, std::uint32_t e) {
        nlohmann::json merged = nlohmann::json::object();
        if (!w.has<CMechanicBinding>(e)) return merged;
        const auto& bind = w.get<CMechanicBinding>(e);
        for (const auto& entry : bind.entries) {
            // We accept params from our own id and from a generic construct core bridge key if present
            bool ours = (entry.id == id());
            if (!ours) continue;
            nlohmann::json pj = parse_params_safe(entry.params_json);
            if (!pj.is_object()) continue;
            for (auto it = pj.begin(); it != pj.end(); ++it) merged[it.key()] = it.value();
        }
        return merged;
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.traversal_constructs_extension", ptv::TraversalConstructsExtension)
