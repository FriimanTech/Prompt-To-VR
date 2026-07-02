#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <cmath>

#include "ground_sprint_overlay_shared.h"

namespace ptv {

class GroundSprintOverlay : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world; (void)entity;
        // Cache per-entity tunables from binding params (if any provided at attach-time)
        auto& T = tunables_[entity];
        T.speed_multiplier   = params.value("sprint_speed_multiplier",   1.6f);
        T.accel_tau_s        = params.value("sprint_accel_tau_s",        0.10f);
        T.decel_tau_s        = params.value("sprint_decel_tau_s",        0.18f);
        T.max_duration_s     = params.value("sprint_max_duration_s",     2.0f);
        T.cooldown_s         = params.value("sprint_cooldown_s",         2.0f);
        T.vignette_on        = params.value("vignette_on_sprint",        true);
        T.vignette_override  = params.value("vignette_intensity_override",-1.0f);
        T.deadzone_override  = params.value("deadzone_override",         -1.0f);
        (void)state_[entity];
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;
        auto& S = state_[entity];
        auto& T = tunables_[entity];

        // Read dynamic inputs from our CMechanicBinding entry (test harness drives these each frame)
        bool slide_active = true;
        bool grounded = true;
        bool left_stick_click = false;
        float left_stick_mag = 0.0f;
        float base_deadzone = fetch_base_deadzone_from_slide(world, entity, 0.18f);
        float dz = (T.deadzone_override >= 0.0f ? T.deadzone_override : base_deadzone);
        float comfort_vignette_default = 0.25f;

        if (world.has<CMechanicBinding>(entity)) {
            const auto& mb = world.get<CMechanicBinding>(entity);
            for (const auto& e : mb.entries) {
                if (e.id == id()) {
                    auto j = nlohmann::json::parse(e.params_json, nullptr, false);
                    if (j.is_object()) {
                        slide_active = j_get_bool(j, "is_slide_active", slide_active);
                        grounded     = j_get_bool(j, "grounded", grounded);
                        left_stick_click = j_get_bool(j, "left_stick_click", left_stick_click);
                        left_stick_mag   = j_get_float(j, "left_stick_mag", left_stick_mag);
                        if (T.deadzone_override < 0.0f)
                            dz = j_get_float(j, "deadzone_base", dz);
                        if (T.vignette_override < 0.0f)
                            comfort_vignette_default = j_get_float(j, "comfort_vignette_default", comfort_vignette_default);
                    }
                    break;
                }
            }
        }

        // Ground check fallback: if no param supplied and physics exists, treat y near floor as grounded (very coarse)
        if (!world.has<CMechanicBinding>(entity) && world.has<CTransform>(entity)) {
            const auto& tr = world.get<CTransform>(entity).t;
            grounded = (tr.position.y <= 0.02f);
        }

        // Block sprint if slide not active or not grounded
        if (!slide_active || !grounded) {
            if (S.sprint_active) on_sprint_end(world, entity, S);
            S.cooldown_t = std::max(S.cooldown_t, 0.0f); // keep running if any
        }

        // Detect press (edge)
        bool press = (left_stick_click && !S.prev_click);
        S.prev_click = left_stick_click;

        // Attempt to start sprint
        if (press && slide_active && grounded && S.cooldown_t <= 0.0f && !S.sprint_active && left_stick_mag > dz) {
            S.sprint_active = true;
            S.t_in_sprint = 0.0f;
            publish_haptics(world, entity, true /*start*/);
        }

        // Target values
        float target_scalar = 1.0f;
        float target_vignette = 0.0f;

        if (S.sprint_active) {
            S.t_in_sprint += dt;
            // auto-end conditions
            if (S.t_in_sprint >= T.max_duration_s || left_stick_mag < dz || !grounded || !slide_active) {
                on_sprint_end(world, entity, S);
            }
        }

        if (S.sprint_active) {
            target_scalar = std::max(1.0f, T.speed_multiplier);
            if (T.vignette_on) {
                float vbase = (T.vignette_override >= 0.0f ? T.vignette_override : comfort_vignette_default);
                target_vignette = std::clamp(vbase, 0.0f, 1.0f);
            }
        }

        // Smooth towards targets
        const float tau_up = T.accel_tau_s;
        const float tau_dn = T.decel_tau_s;
        float tau_scalar = (target_scalar >= S.speed_scalar ? tau_up : tau_dn);
        float tau_vign   = (target_vignette >= S.vignette_level ? tau_up : tau_dn);
        S.speed_scalar   = approach_exp(S.speed_scalar,   target_scalar, dt, tau_scalar);
        S.vignette_level = approach_exp(S.vignette_level, target_vignette, dt, tau_vign);

        // Cooldown timer
        if (S.cooldown_t > 0.0f) {
            S.cooldown_t = std::max(0.0f, S.cooldown_t - dt);
        }

        // Publish effects for consumers: locomotion scalar + vignette level
        publish_loc_scalar(world, entity, S.speed_scalar);
        if (T.vignette_on) publish_vignette(world, entity, S.vignette_level);
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Clear any lingering boost by publishing neutral values once
        auto itS = state_.find(entity);
        if (itS != state_.end()) {
            publish_loc_scalar(world, entity, 1.0f);
            publish_vignette(world, entity, 0.0f);
            state_.erase(itS);
        }
        tunables_.erase(entity);
    }

    std::string id() const override { return "generated.ground_sprint_overlay"; }

private:
    std::unordered_map<std::uint32_t, SprintState> state_{};
    std::unordered_map<std::uint32_t, SprintTunables> tunables_{};

    static void ensure_tag(World& w, std::uint32_t e) {
        if (!w.has<CTag>(e)) w.emplace_or_replace<CTag>(e, CTag{});
    }

    void publish_haptics(World& w, std::uint32_t e, bool start) {
        ensure_tag(w, e);
        auto& tag = w.get<CTag>(e);
        // Protocol from haptics_manager exports (publish prefix and pattern ids)
        nlohmann::json j;
        j["pattern_id"] = start ? "sprint_start" : "sprint_end";
        j["hand"] = "both";
        j["intensity_scale"] = start ? 1.0f : 0.6f;
        j["duration_s"] = start ? 0.10f : 0.08f;
        tag.tags.push_back(std::string("haptics.publish:") + j.dump());
    }

    void on_sprint_end(World& w, std::uint32_t e, SprintState& S) {
        if (!S.sprint_active) return;
        S.sprint_active = false;
        S.t_in_sprint = 0.0f;
        // start cooldown using tunables of this entity
        auto it = tunables_.find(e);
        float cd = (it != tunables_.end() ? std::max(0.0f, it->second.cooldown_s) : 2.0f);
        S.cooldown_t = std::max(S.cooldown_t, cd);
        publish_haptics(w, e, false /*end*/);
    }

    void publish_loc_scalar(World& w, std::uint32_t e, float scalar) {
        ensure_tag(w, e);
        auto& tag = w.get<CTag>(e);
        nlohmann::json j; j["multiplier"] = scalar;
        tag.tags.push_back(std::string("locomotion.scalar:") + j.dump());
    }

    void publish_vignette(World& w, std::uint32_t e, float level) {
        ensure_tag(w, e);
        auto& tag = w.get<CTag>(e);
        nlohmann::json j; j["intensity"] = std::clamp(level, 0.0f, 1.0f);
        tag.tags.push_back(std::string("comfort.vignette:") + j.dump());
    }

    float fetch_base_deadzone_from_slide(World& w, std::uint32_t e, float fallback) {
        if (!w.has<CMechanicBinding>(e)) return fallback;
        const auto& mb = w.get<CMechanicBinding>(e);
        for (const auto& en : mb.entries) {
            if (en.id == "builtin.locomotion.slide.head_relative" || en.id == "builtin.locomotion.slide.controller_relative") {
                auto j = nlohmann::json::parse(en.params_json, nullptr, false);
                if (j.is_object() && j.contains("deadzone")) {
                    try { return std::clamp(j.at("deadzone").get<float>(), 0.0f, 1.0f); } catch(...) {}
                }
            }
        }
        return fallback;
    }
};

} // namespace ptv
