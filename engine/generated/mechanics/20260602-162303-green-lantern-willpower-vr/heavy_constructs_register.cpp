#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>
#include <cmath>
#include "heavy_shared.h"

namespace ptv {

// Forward decls of per-form spawners (defined inline in their cpp units and ODR-used here)
inline void spawn_turret(World& w, std::uint32_t hand_e);
inline void spawn_mech_arm(World& w, std::uint32_t hand_e);
inline void spawn_jet_drill(World& w, std::uint32_t hand_e);
inline void spawn_fist_swarm(World& w, std::uint32_t hand_e);

class HeavySignatureConstructs final : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        // Read configurable thresholds and costs
        state_.cfg.willpower_focus_required = params.value("willpower_focus_required", 0.6f);
        state_.cfg.cooldown_s = params.value("cooldown_s", 20.0f);
        // Per-construct costs (optional nested object or flat fallbacks)
        if (params.contains("per_construct_costs") && params.at("per_construct_costs").is_object()) {
            const auto& c = params.at("per_construct_costs");
            state_.cfg.cost_turret    = c.value("turret",    8.0f);
            state_.cfg.cost_mech_arm  = c.value("mech_arm",  10.0f);
            state_.cfg.cost_jet_drill = c.value("jet_drill", 12.0f);
            state_.cfg.cost_fist_swarm= c.value("fist_swarm",10.0f);
        }
        // Initial HUD push so player sees zeroed cooldowns
        publish_cooldowns(w, e, state_.cd);
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        // 1) Decrement cooldowns and keep HUD updated at a modest cadence
        decay_cooldowns(dt);
        hud_cooldown_accum_ += dt;
        if (hud_cooldown_accum_ > 0.2f) { // throttle HUD tag spam
            publish_cooldowns(w, e, state_.cd);
            hud_cooldown_accum_ = 0.0f;
        }

        // 2) Read live control flags from our own CMechanicBinding entry (params_json is a string)
        nlohmann::json frame{};
        if (w.has<CMechanicBinding>(e)) {
            const auto& mb = w.get<CMechanicBinding>(e);
            for (const auto& entry : mb.entries) {
                if (entry.id == id() || entry.builtin_id == id()) {
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if (j.is_object()) frame = std::move(j);
                    break;
                }
            }
        }

        // Expected optional keys in frame json:
        //   active_heavy_form : string
        //   willpower_focus   : float [0..1]
        //   right_trigger     : { held:bool, released:bool }
        //   left_grip_held    : bool (stabilization)
        // If not provided, we assume nothing pressed.
        const std::string form_str = frame.contains("active_heavy_form") && frame.at("active_heavy_form").is_string()
                                        ? frame.at("active_heavy_form").get<std::string>()
                                        : std::string();
        const HeavyForm sel_form = parse_form(form_str);
        const float focus = frame.contains("willpower_focus") && frame.at("willpower_focus").is_number()
                                ? frame.at("willpower_focus").get<float>()
                                : state_.last_focus_sample; // hold last known
        state_.last_focus_sample = focus;

        nlohmann::json rt = frame.contains("right_trigger") ? frame.at("right_trigger") : nlohmann::json::object();
        const bool rt_held = json_bool(rt, "held", false);
        const bool rt_released = json_bool(rt, "released", false);
        const bool left_grip = json_bool(frame, "left_grip_held", false);

        // Two-hand stabilization smoothing (0 -> 1 quickly while held, decays when released)
        const float target_stab = left_grip ? 1.0f : 0.0f;
        state_.stab_smoother += (target_stab - state_.stab_smoother) * std::min(1.0f, dt * 10.0f);

        // 3) Charging gate: must have a valid selection, pass focus threshold, and be off cooldown
        const bool focus_ok = (focus >= state_.cfg.willpower_focus_required);
        const bool cd_ok = is_cooldown_ready(sel_form);
        const bool can_charge = (sel_form != HeavyForm::None) && focus_ok && cd_ok;

        if (rt_held && can_charge) {
            state_.charge.charging = true;
            state_.charge.form = sel_form;
            state_.charge.t += dt;
            // telegraphed charge-up: intensity eases in; stabilization reduces jitter -> raises perceived intensity
            float base = std::clamp(state_.charge.t / charge_time_s_, 0.0f, 1.0f);
            state_.charge.preview_intensity = std::clamp(base * (0.85f + 0.15f * state_.stab_smoother), 0.0f, 1.0f);
            publish_charge_vfx(w, e, state_.charge.preview_intensity);
            // resonance haptics ticks during charge
            charge_haptics_accum_ += dt;
            if (charge_haptics_accum_ > 0.25f) {
                publish_haptics(w, e, "recharge_rhythm", "both", 0.5f + 0.4f * state_.charge.preview_intensity, 0.05f);
                charge_haptics_accum_ = 0.0f;
            }
        }

        // 4) Release to deploy if fully charged
        if (rt_released) {
            if (state_.charge.charging && state_.charge.preview_intensity >= 0.999f && sel_form == state_.charge.form) {
                deploy(w, e, sel_form);
            }
            // reset charge either way
            state_.charge = ChargeState{};
        }

        // 5) Safety: stop charge if the selection changed or focus dropped below threshold
        if (!rt_held || !can_charge || sel_form != state_.charge.form) {
            state_.charge = ChargeState{};
        }
    }

    void on_detach(World&, std::uint32_t) override {}
    std::string id() const override { return "generated.heavy_signature_constructs"; }

private:
    void decay_cooldowns(float dt) {
        auto dec = [dt](float& v){ v = std::max(0.0f, v - dt); };
        dec(state_.cd.turret);
        dec(state_.cd.mech_arm);
        dec(state_.cd.jet_drill);
        dec(state_.cd.fist_swarm);
    }

    bool is_cooldown_ready(HeavyForm f) const {
        switch (f) {
            case HeavyForm::Turret: return state_.cd.turret     <= 0.0f;
            case HeavyForm::MechArm: return state_.cd.mech_arm  <= 0.0f;
            case HeavyForm::JetDrill: return state_.cd.jet_drill<= 0.0f;
            case HeavyForm::FistSwarm:return state_.cd.fist_swarm<=0.0f;
            default: return false;
        }
    }

    void set_cooldown(HeavyForm f, float v) {
        switch (f) {
            case HeavyForm::Turret: state_.cd.turret = v; break;
            case HeavyForm::MechArm: state_.cd.mech_arm = v; break;
            case HeavyForm::JetDrill: state_.cd.jet_drill = v; break;
            case HeavyForm::FistSwarm: state_.cd.fist_swarm = v; break;
            default: break;
        }
    }

    void deploy(World& w, std::uint32_t e, HeavyForm f) {
        // Spend cost, fire haptics, broadcast spawn and special hooks
        switch (f) {
            case HeavyForm::Turret:
                publish_consume_once(w, e, state_.cfg.cost_turret);
                spawn_turret(w, e);
                // drain while alive hint for turret (handled by turret AI when it claims/creates an entity)
                break;
            case HeavyForm::MechArm:
                publish_consume_once(w, e, state_.cfg.cost_mech_arm);
                spawn_mech_arm(w, e);
                break;
            case HeavyForm::JetDrill:
                publish_consume_once(w, e, state_.cfg.cost_jet_drill);
                spawn_jet_drill(w, e);
                // Suggest sustained drain while active
                publish_drain_per_s(w, e, 4.0f);
                break;
            case HeavyForm::FistSwarm:
                publish_consume_once(w, e, state_.cfg.cost_fist_swarm);
                spawn_fist_swarm(w, e);
                break;
            default: break;
        }
        // Global deploy haptics burst and cooldown apply
        publish_haptics(w, e, "construct_form_heavy", "both", 1.0f, 0.2f);
        set_cooldown(f, state_.cfg.cooldown_s);
        // Broadcast to HUD immediately after deploy
        publish_cooldowns(w, e, state_.cd);
        // Acceptance hook tags
        add_tag(w, e, std::string("test_heavy_construct_cooldown:") + nlohmann::json({{"ok", true}}).dump());
        if (f == HeavyForm::JetDrill) {
            add_tag(w, e, std::string("test_boss_phase_break_armor:") + nlohmann::json({{"jet_drill", true}}).dump());
        }
    }

private:
    HeavyState state_{};
    float charge_time_s_{1.2f};
    float hud_cooldown_accum_{0.0f};
    float charge_haptics_accum_{0.0f};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.heavy_signature_constructs", ptv::HeavySignatureConstructs)
