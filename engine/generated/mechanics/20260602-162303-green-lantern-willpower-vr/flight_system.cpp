#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <optional>
#include <vector>
#include <cmath>
#include <algorithm>

// Forward declarations for haptics exported by haptics_manager task.
namespace ptv { namespace haptics {
    void publish(const std::string&, const std::string&, float, std::optional<float>);
    bool has_pattern(const std::string&);
    float get_user_intensity();
} }

namespace ptv {

// Small math helpers (do not assume operator overloads on Vec3).
static inline Vec3 vadd(const Vec3& a, const Vec3& b){ return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
static inline Vec3 vsub(const Vec3& a, const Vec3& b){ return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static inline Vec3 vscale(const Vec3& a, float s){ return Vec3{a.x * s, a.y * s, a.z * s}; }
static inline float vdot(const Vec3& a, const Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float vlen2(const Vec3& a){ return vdot(a,a); }
static inline float vlen(const Vec3& a){ float L2 = vlen2(a); return L2 > 0.0f ? std::sqrt(L2) : 0.0f; }
static inline Vec3 vnorm(const Vec3& a){ float L = vlen(a); return (L>1e-5f) ? vscale(a, 1.0f/L) : Vec3{0,0,0}; }
static inline Vec3 vclamp_len(const Vec3& a, float maxL){ float L = vlen(a); if (L<=maxL || L<=1e-5f) return a; return vscale(a, maxL/L); }
static inline float smooth_tau(float current, float target, float tau, float dt){ if (tau<=1e-5f) return target; float a = 1.0f - std::exp(-dt/tau); return current + (target-current)*a; }
static inline Vec3 smooth_tau_vec(const Vec3& cur, const Vec3& tgt, float tau, float dt){ return Vec3{ smooth_tau(cur.x,tgt.x,tau,dt), smooth_tau(cur.y,tgt.y,tau,dt), smooth_tau(cur.z,tgt.z,tau,dt) }; }
static inline float lerp(float a, float b, float t){ return a + (b-a)*t; }
static inline Vec3 lerp3(const Vec3& a, const Vec3& b, float t){ return Vec3{ lerp(a.x,b.x,t), lerp(a.y,b.y,t), lerp(a.z,b.z,t) }; }

// Input snapshot (driven by test harness via CMechanicBinding.params_json on the bound entity).
struct FlightInput {
    // Left stick in chosen frame (x = right, y = forward), already deadzoned/scaled by harness.
    float ls_x{0.0f};
    float ls_y{0.0f};
    bool  ls_click{false}; // boost modifier
    float rs_y{0.0f};      // altitude/pitch assist
    bool  a_double_tap{false}; // toggle flight/hover or brake-to-hover
    bool  b_press{false};      // quick evade burst
    // Frame bases injected by harness (head/controller). Fallbacks provided if absent.
    Vec3  fwd{0.0f, 0.0f, 1.0f};
    Vec3  right{1.0f, 0.0f, 0.0f};
    Vec3  up{0.0f, 1.0f, 0.0f};
    // Mode toggles (ui/preferences may set these booleans per-frame via params).
    bool  altitude_assist{true};
    bool  pitch_assist{false};
    bool  in_no_fly{false};
};

struct Tuning {
    float cruise_max_speed{6.0f};
    float boost_mul{1.6f};
    float accel_tau{0.20f};
    float decel_tau{0.25f};
    float altitude_rate{2.0f};
    bool  bank_assist{true};
    bool  vignette_on_boost{true};
    float oob_return_delay{1.0f};
    float soft_bounce_coeff{0.35f};
    std::string no_fly_tag{"no_fly"};
    // Shared tag keys (from other mechanics). Injected if available.
    std::string vignette_tag_key{"comfort_vignette"}; // from ground_sprint_overlay exports
};

struct OOBLerp {
    bool active{false};
    float t{0.0f};
    Vec3 start{};
    Vec3 target{};
};

struct PerEntityState {
    bool  in_flight{false};
    bool  boosting{false};
    Vec3  vel{0.0f,0.0f,0.0f};
    Vec3  last_safe_pos{0.0f,0.0f,0.0f};
    OOBLerp oob; // out-of-bounds return state
};

class FlightSystem final : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t e, const nlohmann::json& params) override {
        // Cache tuning per-mechanic instance (same for all attached entities for this instance).
        tuning_.cruise_max_speed = params.value("cruise_max_speed_mps", 6.0f);
        tuning_.boost_mul        = params.value("boost_multiplier", 1.6f);
        tuning_.accel_tau        = params.value("accel_tau_s", 0.20f);
        tuning_.decel_tau        = params.value("decel_tau_s", 0.25f);
        tuning_.altitude_rate    = params.value("altitude_rate_mps", 2.0f);
        tuning_.bank_assist      = params.value("bank_assist", true);
        tuning_.vignette_on_boost= params.value("comfort_vignette_on_boost", true);
        tuning_.oob_return_delay = params.value("oob_return_delay_s", 1.0f);
        tuning_.soft_bounce_coeff= params.value("collision_soft_bounce_coeff", 0.35f);
        tuning_.no_fly_tag       = params.value("no_fly_volumes_tag", std::string("no_fly"));

        // Try to learn vignette_tag_key if another mechanic exported it via CTag on spawn entity.
        // We piggyback on CTag by convention: if present and contains known key, we reuse it.
        // Else we keep default "comfort_vignette" which is provided by ground_sprint_overlay.
        if (world.has<CTag>(e)) {
            const auto& tags = world.get<CTag>(e).tags;
            for (const auto& t : tags) {
                if (t == "comfort_vignette") { tuning_.vignette_tag_key = t; break; }
            }
        }
        // Initialize per-entity state.
        (void)state_for(e); // creates default if absent
    }

    void on_update(World& world, std::uint32_t e, float dt) override {
        if (!world.valid(e)) return;
        auto it = states_.find(e);
        if (it == states_.end()) it = states_.emplace(e, PerEntityState{}).first;
        auto& st = it->second;

        // Read transform; if missing we cannot move.
        if (!world.has<CTransform>(e)) return;
        auto& tr = world.get<CTransform>(e).t;

        // Pull input snapshot from this entity's CMechanicBinding entry for this mechanic id.
        FlightInput in = read_inputs_from_binding(world, e);

        // 1) Toggle into flight if A double-tap and not in a no-fly area.
        if (in.a_double_tap && !in.in_no_fly) {
            st.in_flight = !st.in_flight;
            if (!st.in_flight) {
                // Snapping to hover: kill velocity.
                st.vel = Vec3{0,0,0};
                mark_hover_requested(world, e);
            }
        }

        // 2) Compute desired thrust direction and target speed.
        Vec3 basis_fwd  = vnorm(in.fwd);
        Vec3 basis_right= vnorm(in.right);
        Vec3 basis_up   = vnorm(in.up);
        Vec3 planar = vadd( vscale(basis_right, in.ls_x), vscale(basis_fwd, in.ls_y) );
        Vec3 thrust_dir = vnorm(planar);
        float target_speed = tuning_.cruise_max_speed;

        // Altitude assist (Right stick Y) — adjust vertical velocity component directly when in flight.
        Vec3 altitude_vel{0,0,0};
        if (st.in_flight && std::fabs(in.rs_y) > 1e-3f && in.altitude_assist) {
            altitude_vel = vscale(basis_up, tuning_.altitude_rate * in.rs_y);
        }

        // Boost when left stick click held during flight and there is some thrust direction.
        bool wants_boost = st.in_flight && in.ls_click && (vlen(thrust_dir) > 0.1f);
        if (wants_boost && !st.boosting) on_boost_begin(world, e);
        if (!wants_boost && st.boosting) on_boost_end(world, e);
        st.boosting = wants_boost;
        if (st.boosting) target_speed *= tuning_.boost_mul;

        // Choose accel vs decel tau for planar speed convergence.
        float current_planar_speed = std::sqrt(std::max(0.0f, vlen2(st.vel) - st.vel.y*st.vel.y));
        float used_tau = ( (target_speed > current_planar_speed) ? tuning_.accel_tau : tuning_.decel_tau );

        // Desired planar velocity.
        Vec3 desired_planar = vscale(thrust_dir, target_speed);
        // Smooth converge.
        Vec3 new_planar = smooth_tau_vec(Vec3{st.vel.x, 0.0f, st.vel.z}, desired_planar, used_tau, dt);

        // Compose final velocity with altitude component.
        Vec3 desired_vel = Vec3{ new_planar.x, st.vel.y, new_planar.z };
        // Apply altitude assist directly (smooth blend for comfort).
        desired_vel.y = smooth_tau(st.vel.y, altitude_vel.y, 0.12f, dt);

        // Brake intent: if in flight and thumbstick released and not boosting, ease to hover.
        bool stick_released = (std::fabs(in.ls_x) < 0.05f && std::fabs(in.ls_y) < 0.05f);
        if (st.in_flight && stick_released && !st.boosting) {
            desired_vel = smooth_tau_vec(st.vel, Vec3{0,0,0}, tuning_.decel_tau, dt);
            // If we've slowed almost to zero, flip to hover immediately.
            if (vlen(desired_vel) < 0.05f) {
                st.in_flight = false;
                desired_vel = Vec3{0,0,0};
                mark_hover_requested(world, e);
            }
        }

        // Quick-evade on B: short lateral impulse to HMD right if no threat vector provided.
        if (in.b_press && st.in_flight) {
            Vec3 lateral = basis_right; // opposite incoming fire could be provided in params later.
            float evade_impulse = 3.0f; // m/s instant impulse
            desired_vel = vadd(desired_vel, vscale(lateral, evade_impulse));
            publish_haptics_if(world, e, "evade_burst", 1.0f);
            // Brief comfort vignette tag pulse.
            tag_once_for_vignette(world, e, true);
        }

        // Update velocity into state (also clamp for sanity in tests).
        st.vel = vclamp_len(desired_vel, tuning_.cruise_max_speed * tuning_.boost_mul * 1.1f);

        // 3) Integrate position if in flight (or if OOB-lerp is active).
        // Track last safe position when within reasonable bounds.
        const float safe_bound = 120.0f; // meters radius
        bool within_bounds = (vlen(tr.position) < safe_bound);
        if (within_bounds) st.last_safe_pos = tr.position;

        // OOB handling: start lerp back to safe on leaving bounds while in flight.
        if (!within_bounds && !st.oob.active) {
            st.oob.active = true;
            st.oob.t = 0.0f;
            st.oob.start = tr.position;
            st.oob.target = st.last_safe_pos;
            // Visual/HUD warning would hook here (TODO: HUD message bus when available).
        }

        if (st.oob.active) {
            st.oob.t += (tuning_.oob_return_delay <= 1e-4f) ? 1.0f : (dt / tuning_.oob_return_delay);
            float t = std::clamp(st.oob.t, 0.0f, 1.0f);
            tr.position = lerp3(st.oob.start, st.oob.target, t);
            // Kill velocity while lerping back to avoid overshoot.
            st.vel = Vec3{0,0,0};
            if (t >= 1.0f) {
                st.oob.active = false;
                st.in_flight = false; // auto-hover at safe point
                mark_hover_requested(world, e);
            }
        } else if (st.in_flight) {
            // Collisions/soft bounce (very conservative: simple ground-plane bounce at y=0).
            Vec3 next = vadd(tr.position, vscale(st.vel, dt));
            // Predictive simple cast on ground plane.
            if (next.y < 0.0f) {
                next.y = std::abs(next.y) * tuning_.soft_bounce_coeff; // bounce upward mildly
                st.vel.y = std::abs(st.vel.y) * tuning_.soft_bounce_coeff;
            }
            tr.position = next;
        }

        // 4) Comfort hooks: vignette while boosting, clear when not.
        if (tuning_.vignette_on_boost) tag_vignette(world, e, st.boosting);
        // No camera roll rule is satisfied (we never modify rotation roll). Optional mild stabilization could blend rotation here if exposed later.

        // Maintain a small identifying tag while in flight for other systems (debug/acceptance).
        maintain_flight_tag(world, e, st.in_flight);
    }

    void on_detach(World& world, std::uint32_t e) override {
        // Clear comfort tag if we set it.
        if (world.has<CTag>(e)) {
            auto& tags = world.get<CTag>(e).tags;
            tags.erase(std::remove(tags.begin(), tags.end(), tuning_.vignette_tag_key), tags.end());
            tags.erase(std::remove(tags.begin(), tags.end(), std::string("in_flight")), tags.end());
        }
        states_.erase(e);
    }

    std::string id() const override { return "generated.flight_system"; }

private:
    PerEntityState& state_for(std::uint32_t e){ return states_[e]; }

    void maintain_flight_tag(World& world, std::uint32_t e, bool on){
        if (!world.valid(e)) return;
        if (!world.has<CTag>(e)) { if (on) world.emplace_or_replace<CTag>(e, CTag{{"in_flight"}}); return; }
        auto& tags = world.get<CTag>(e).tags;
        auto it = std::find(tags.begin(), tags.end(), std::string("in_flight"));
        if (on && it==tags.end()) tags.emplace_back("in_flight");
        if (!on && it!=tags.end()) tags.erase(it);
    }

    void mark_hover_requested(World& world, std::uint32_t e){
        // Communicate intent to Hover mechanic via CTag convention.
        if (!world.valid(e)) return;
        if (!world.has<CTag>(e)) { world.emplace_or_replace<CTag>(e, CTag{{"hover_requested"}}); return; }
        auto& tags = world.get<CTag>(e).tags;
        if (std::find(tags.begin(), tags.end(), std::string("hover_requested"))==tags.end()) tags.emplace_back("hover_requested");
        // Also clear flight vignette.
        tag_vignette(world, e, false);
    }

    void tag_vignette(World& world, std::uint32_t e, bool on){
        if (!world.valid(e)) return;
        if (!world.has<CTag>(e)) { if (on) world.emplace_or_replace<CTag>(e, CTag{{tuning_.vignette_tag_key}}); return; }
        auto& tags = world.get<CTag>(e).tags;
        auto it = std::find(tags.begin(), tags.end(), tuning_.vignette_tag_key);
        if (on && it==tags.end()) tags.emplace_back(tuning_.vignette_tag_key);
        if (!on && it!=tags.end()) tags.erase(it);
    }

    void tag_once_for_vignette(World& world, std::uint32_t e, bool pulse){
        if (!pulse) return; tag_vignette(world, e, true); /* cleared implicitly by next state change */
    }

    void on_boost_begin(World& world, std::uint32_t e){ publish_haptics_if(world, e, "boost_hum", 0.7f); }
    void on_boost_end(World& world, std::uint32_t){ /* could stop a looping pattern if supported later */ }

    void publish_haptics_if(World& /*world*/, std::uint32_t /*e*/, const std::string& pattern, float amp){
        // Query presence and publish with user intensity scaling; safe if haptics manager not present (linker no-op in tests).
        try {
            if (ptv::haptics::has_pattern(pattern)) {
                float user = std::clamp(ptv::haptics::get_user_intensity(), 0.0f, 1.0f);
                ptv::haptics::publish(pattern, "both", std::clamp(amp*user, 0.0f, 1.0f), std::nullopt);
            }
        } catch (...) { /* offline_ok: ignore if service unavailable */ }
    }

    FlightInput read_inputs_from_binding(World& world, std::uint32_t e){
        FlightInput out{};
        if (!world.has<CMechanicBinding>(e)) return out;
        const auto& mb = world.get<CMechanicBinding>(e);
        for (const auto& entry : mb.entries) {
            // Match this mechanic id exactly.
            if (!(entry.id == id() || entry.builtin_id == id())) continue;
            auto j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) continue;
            if (j.contains("ls")) {
                auto v = j.at("ls"); if (v.is_array() && v.size()>=2) { out.ls_x = v.at(0).get<float>(); out.ls_y = v.at(1).get<float>(); }
            }
            out.ls_click    = j.value("ls_click", false);
            out.rs_y        = j.value("rs_y", 0.0f);
            out.a_double_tap= j.value("a_double_tap", false);
            out.b_press     = j.value("b_press", false);
            out.altitude_assist = j.value("altitude_assist", true);
            out.pitch_assist    = j.value("pitch_assist", false);
            out.in_no_fly       = j.value("in_no_fly", false);
            if (j.contains("frame") && j["frame"].is_object()) {
                const auto& fr = j["frame"];
                if (fr.contains("fwd"))  { auto v=fr["fwd"]; if (v.is_array()&&v.size()>=3) out.fwd  = Vec3{v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()}; }
                if (fr.contains("right")){ auto v=fr["right"];if (v.is_array()&&v.size()>=3) out.right= Vec3{v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()}; }
                if (fr.contains("up"))   { auto v=fr["up"];   if (v.is_array()&&v.size()>=3) out.up   = Vec3{v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()}; }
            }
            // Only the first matching entry is used.
            break;
        }
        return out;
    }

private:
    Tuning tuning_{};
    std::unordered_map<std::uint32_t, PerEntityState> states_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.flight_system", ptv::FlightSystem)
