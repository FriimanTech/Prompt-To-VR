#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <cmath>

// Minimal forward decls for haptics dependency (provided by haptics_manager task)
namespace ptv { namespace haptics {
    enum class HandMask { Left=1, Right=2, Both=3 };
    // exports list indicates these exist; declare to link
    bool has_pattern(const std::string&);
    void publish(const std::string& pattern, const std::string& hand, float intensity, std::optional<float> duration_s);
    float get_user_intensity();
}}

// Cross-file helper declarations (implemented in other .cpp files of this mechanic)
namespace control_ext_helpers {
    struct TargetPick { bool found{false}; std::uint32_t entity{0}; float dist{0.0f}; };
    TargetPick pick_target_forward_cone(ptv::World& w, std::uint32_t from_entity,
                                        float max_distance_m,
                                        float half_angle_deg,
                                        bool prefer_enemies);
    bool entity_has_tag(ptv::World& w, std::uint32_t e, std::string_view tag_sv);
    void ensure_core_control_entries(ptv::World& w, std::uint32_t hand_entity);
    void vfx_snap_tether(ptv::World& w, const ptv::Vec3& pos_world);
}

namespace ptv {

class ControlConstructsExtension : public IMechanic {
public:
    std::string id() const override { return "generated.control_constructs_extension"; }

    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        auto& S = state_[e];
        S.params = Params::from_json(params);
        S.last_right_trigger_state = false;
        S.last_left_trigger_state = false;
        S.last_left_grip_state = false;
        S.mode = Mode::Idle;
        S.tether.target = entt::null;
        S.tether.current_len = S.params.tether_max_length_m;
        S.tether.prev_target_pos_valid = false;
        S.time = 0.0f;
        // Inject our Control entries into the construct core wheel on this hand.
        control_ext_helpers::ensure_core_control_entries(w, e);
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        auto it = state_.find(e);
        if (it == state_.end()) return;
        auto& S = it->second;
        S.time += dt;

        // Read live params from our own binding entry each frame (test harness will drive these booleans)
        bool left_trigger_held=false, right_trigger=false, left_grip_held=false;
        std::string active_control = "chain_tether"; // default
        read_live_inputs_from_binding(w, e, left_trigger_held, right_trigger, left_grip_held, active_control);

        // Rising edge on right trigger
        const bool right_trigger_pressed = right_trigger && !S.last_right_trigger_state;

        if (active_control == "chain_tether") {
            tick_chain_tether(w, e, dt, S, left_trigger_held, left_grip_held, right_trigger_pressed);
        } else if (active_control == "restraining_bands") {
            tick_restraining_bands(w, e, dt, S, right_trigger_pressed);
        } else if (active_control == "energy_cage_control") {
            tick_cage_enhance(w, e, dt, S, left_trigger_held);
        }

        S.last_right_trigger_state = right_trigger;
        S.last_left_trigger_state = left_trigger_held;
        S.last_left_grip_state = left_grip_held;

        // Global decay for restraining bands stacks when not refreshed
        decay_band_stacks(w, dt, S);
    }

    void on_detach(World& w, std::uint32_t e) override {
        // Clean up local state; no world components were created outside existing ones
        state_.erase(e);
    }

private:
    enum class Mode { Idle, TetherActive };

    struct Params {
        float tether_max_length_m = 18.0f;
        float tether_tension_break_threshold = 1.0f; // normalized
        float reel_in_speed_mps = 2.0f;
        float bands_slow_per_stack = 0.18f;
        int   bands_max_stacks = 3;
        float fear_zone_resistance_multiplier = 1.5f; // increases break chance in yellow zones
        static Params from_json(const nlohmann::json& j) {
            Params p;
            if (j.is_object()) {
                p.tether_max_length_m = j.value("tether_max_length_m", p.tether_max_length_m);
                p.tether_tension_break_threshold = j.value("tether_tension_break_threshold", p.tether_tension_break_threshold);
                p.reel_in_speed_mps = j.value("reel_in_speed_mps", p.reel_in_speed_mps);
                p.bands_slow_per_stack = j.value("bands_slow_per_stack", p.bands_slow_per_stack);
                p.bands_max_stacks = j.value("bands_max_stacks", p.bands_max_stacks);
                p.fear_zone_resistance_multiplier = j.value("fear_zone_resistance_multiplier", p.fear_zone_resistance_multiplier);
            }
            return p;
        }
    };

    struct TetherState {
        entt::entity target{entt::null};
        float current_len{18.0f};
        float tension_norm{0.0f};
        bool anchored_to_world{false};
        ptv::Vec3 prev_target_pos{};
        bool prev_target_pos_valid{false};
    };

    struct BandsData { int stacks{0}; float time_since_refresh{0.0f}; };

    struct LocalState {
        Params params;
        Mode mode{Mode::Idle};
        TetherState tether;
        bool last_left_trigger_state{false};
        bool last_right_trigger_state{false};
        bool last_left_grip_state{false};
        float time{0.0f};
        std::unordered_map<std::uint32_t, BandsData> bands; // per-target stacks
    };

    // Live-input reader from our own binding entry on this entity
    static void read_live_inputs_from_binding(World& w, std::uint32_t e,
                                              bool& left_trigger_held,
                                              bool& right_trigger,
                                              bool& left_grip_held,
                                              std::string& active_control)
    {
        left_trigger_held=false; right_trigger=false; left_grip_held=false; active_control = active_control; // keep default
        if (!w.has<CMechanicBinding>(e)) return;
        auto& bind = w.get<CMechanicBinding>(e);
        for (const auto& entry : bind.entries) {
            if (entry.id == "generated.control_constructs_extension") {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object()) {
                    left_trigger_held = j.value("left_trigger_held", left_trigger_held);
                    right_trigger     = j.value("right_trigger", right_trigger);
                    left_grip_held    = j.value("left_grip_held", left_grip_held);
                    active_control    = j.value("active_control", active_control);
                }
                break;
            }
        }
    }

    // Chain/Tether tick
    static void tick_chain_tether(World& w, std::uint32_t hand_e, float dt, LocalState& S,
                                  bool left_trigger_held, bool /*left_grip_held*/, bool right_trigger_pressed)
    {
        // Fire tether on press if idle
        if (right_trigger_pressed && S.tether.target == entt::null) {
            // Prefer enemies; 25m, 25deg cone
            auto pick = control_ext_helpers::pick_target_forward_cone(w, hand_e, S.params.tether_max_length_m, 25.0f, /*prefer_enemies=*/true);
            if (pick.found) {
                S.tether.target = static_cast<entt::entity>(pick.entity);
                S.tether.current_len = std::min(pick.dist, S.params.tether_max_length_m);
                S.mode = Mode::TetherActive;
                // Haptics confirmation
                if (ptv::haptics::has_pattern("sharp")) {
                    ptv::haptics::publish("sharp", "Right", 0.6f * ptv::haptics::get_user_intensity(), 0.05f);
                }
            }
        }

        if (S.tether.target != entt::null && !w.valid(static_cast<std::uint32_t>(S.tether.target))) {
            S.tether.target = entt::null; S.mode = Mode::Idle; S.tether.prev_target_pos_valid=false; return;
        }

        if (S.tether.target != entt::null) {
            // Compute positions
            if (!w.has<CTransform>(hand_e)) { S.tether.target = entt::null; S.mode=Mode::Idle; return; }
            auto& hand_tr = w.get<CTransform>(hand_e).t;
            auto hand_pos = hand_tr.position;

            auto tgt_e_u32 = static_cast<std::uint32_t>(S.tether.target);
            if (!w.has<CTransform>(tgt_e_u32)) { S.tether.target = entt::null; S.mode=Mode::Idle; return; }
            auto& tgt_tr = w.get<CTransform>(tgt_e_u32).t;
            auto tgt_pos = tgt_tr.position;

            // Distance and direction
            ptv::Vec3 dir{tgt_pos.x - hand_pos.x, tgt_pos.y - hand_pos.y, tgt_pos.z - hand_pos.z};
            float dist = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            if (dist > 1e-3f) { dir.x/=dist; dir.y/=dist; dir.z/=dist; }

            // Relative speed estimate
            float rel_speed = 0.0f;
            if (S.tether.prev_target_pos_valid) {
                ptv::Vec3 dv{ (tgt_pos.x - S.tether.prev_target_pos.x)/std::max(dt,1e-3f),
                              (tgt_pos.y - S.tether.prev_target_pos.y)/std::max(dt,1e-3f),
                              (tgt_pos.z - S.tether.prev_target_pos.z)/std::max(dt,1e-3f) };
                rel_speed = std::sqrt(dv.x*dv.x+dv.y*dv.y+dv.z*dv.z);
            }
            S.tether.prev_target_pos = tgt_pos; S.tether.prev_target_pos_valid=true;

            // Update tether length (reel in if left trigger held)
            if (left_trigger_held) {
                S.tether.current_len = std::max(0.5f, S.tether.current_len - S.params.reel_in_speed_mps * dt);
            } else {
                // small relaxation to avoid constant yank
                S.tether.current_len = std::min(S.tether.current_len + 0.5f*dt, S.params.tether_max_length_m);
            }

            // Apply pull when beyond length (move target toward hand gently; do NOT move the player/camera)
            float over = dist - S.tether.current_len;
            if (over > 0.0f) {
                float pull_m = std::min(over, 2.0f * dt); // capped pull speed for comfort
                tgt_tr.position.x -= dir.x * pull_m;
                tgt_tr.position.y -= dir.y * pull_m;
                tgt_tr.position.z -= dir.z * pull_m;
                // frictional damping visualized as slowing target slightly even when inside length (no physics engine)
            }

            // Compute normalized tension
            float len_eps = std::max(0.2f, S.tether.current_len);
            float stretch = std::max(0.0f, dist - S.tether.current_len);
            float tension_pos = stretch / len_eps; // 0..~
            float tension_vel = rel_speed / 10.0f; // heuristic scale
            float tension = tension_pos + 0.5f * tension_vel;

            // Fear zone multiplier makes break more likely
            bool in_yellow = control_ext_helpers::entity_has_tag(w, hand_e, "yellow_zone") ||
                             control_ext_helpers::entity_has_tag(w, tgt_e_u32, "yellow_zone");
            if (in_yellow) tension *= S.params.fear_zone_resistance_multiplier;

            S.tether.tension_norm = tension;
            if (S.tether.tension_norm > S.params.tether_tension_break_threshold) {
                // Break: snap VFX and clear state
                control_ext_helpers::vfx_snap_tether(w, tgt_tr.position);
                if (ptv::haptics::has_pattern("click")) {
                    ptv::haptics::publish("click", "Right", 0.5f * ptv::haptics::get_user_intensity(), 0.04f);
                }
                S.tether.target = entt::null; S.mode = Mode::Idle; S.tether.prev_target_pos_valid=false;
            }
        }
    }

    // Restraining Bands: add stack on press to current target, with visual tag; stacks decay handled separately
    static void tick_restraining_bands(World& w, std::uint32_t hand_e, float /*dt*/, LocalState& S,
                                       bool right_trigger_pressed)
    {
        if (!right_trigger_pressed) return;
        auto pick = control_ext_helpers::pick_target_forward_cone(w, hand_e, 20.0f, 30.0f, /*prefer_enemies=*/true);
        if (!pick.found) return;
        std::uint32_t tgt = pick.entity;
        auto& bd = S.bands[tgt];
        bd.stacks = std::min(S.params.bands_max_stacks, bd.stacks + 1);
        bd.time_since_refresh = 0.0f;
        // Mark target with a tag for other systems (and for debug/testing)
        if (w.valid(tgt)) {
            if (w.has<CTag>(tgt)) {
                auto& tag = w.get<CTag>(tgt);
                add_or_update_slow_tag(tag, bd.stacks, S.params.bands_slow_per_stack);
            } else {
                CTag t; add_or_update_slow_tag(t, bd.stacks, S.params.bands_slow_per_stack); w.emplace_or_replace<CTag>(tgt, t);
            }
        }
        if (ptv::haptics::has_pattern("sharp")) {
            ptv::haptics::publish("sharp", "Right", 0.5f * ptv::haptics::get_user_intensity(), 0.03f);
        }
    }

    static void tick_cage_enhance(World& w, std::uint32_t hand_e, float /*dt*/, LocalState& /*S*/, bool left_trigger_held)
    {
        // Soft hook: when left trigger is held, we annotate nearest caged target with a hint tag the core could read.
        if (!left_trigger_held) return;
        auto pick = control_ext_helpers::pick_target_forward_cone(w, hand_e, 18.0f, 25.0f, /*prefer_enemies=*/true);
        if (!pick.found) return;
        auto tgt = pick.entity;
        if (!w.valid(tgt)) return;
        // Tag with a transient hint; core may react by tightening cage.
        if (w.has<CTag>(tgt)) {
            auto& tag = w.get<CTag>(tgt);
            if (std::find(tag.tags.begin(), tag.tags.end(), std::string("cage_tighten_hint")) == tag.tags.end())
                tag.tags.push_back("cage_tighten_hint");
        } else {
            CTag t; t.tags.push_back("cage_tighten_hint"); w.emplace_or_replace<CTag>(tgt, t);
        }
    }

    static void decay_band_stacks(World& w, float dt, LocalState& S)
    {
        // Decay when not refreshed for a while
        std::vector<std::uint32_t> toErase;
        for (auto& kv : S.bands) {
            auto tgt = kv.first; auto& bd = kv.second;
            bd.time_since_refresh += dt;
            if (bd.time_since_refresh > 3.0f && bd.stacks > 0) {
                bd.stacks -= 1; bd.time_since_refresh = 0.0f;
                if (w.valid(tgt)) {
                    if (w.has<CTag>(tgt)) {
                        auto& tag = w.get<CTag>(tgt);
                        add_or_update_slow_tag(tag, bd.stacks, S.params.bands_slow_per_stack);
                    }
                }
            }
            if (bd.stacks <= 0) toErase.push_back(tgt);
        }
        for (auto tgt : toErase) S.bands.erase(tgt);
    }

    static void add_or_update_slow_tag(CTag& tag, int stacks, float per_stack)
    {
        // Keep a single tag string like "slow_stacks:x:mult:0.46"
        std::string prefix = "slow_stacks:";
        auto it = std::find_if(tag.tags.begin(), tag.tags.end(), [&](const std::string& s){ return s.rfind(prefix,0)==0; });
        float mult = std::max(0.1f, 1.0f - per_stack * static_cast<float>(stacks));
        std::string value = prefix + std::to_string(stacks) + ":mult:" + std::to_string(mult);
        if (it==tag.tags.end()) tag.tags.push_back(value); else *it = value;
    }

    std::unordered_map<std::uint32_t, LocalState> state_;
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.control_constructs_extension", ptv::ControlConstructsExtension)
