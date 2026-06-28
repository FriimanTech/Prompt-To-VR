#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <cmath>

// Forward decls for haptics dependency (provided by generated.haptics_manager)
namespace ptv { namespace haptics {
    // pattern_id, hand_mask (string like "left"/"right"/"both"), intensity 0..1, duration seconds (optional; pattern-defined if not set)
    void publish(const std::string&, const std::string&, float, std::optional<float>);
    bool has_pattern(const std::string&);
    void set_user_intensity(float);
    float get_user_intensity();
    std::string pattern_to_string(int); // not used here
    std::string hand_to_string(int);    // not used here
}} // namespace ptv::haptics

namespace ptv {

class GroundSprintOverlay final : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        // Config with sensible, comfort-oriented defaults
        sprint_speed_multiplier_ = clampf(params.value("sprint_speed_multiplier", 1.6f), 1.0f, 3.0f);
        accel_tau_s_ = clampf(params.value("sprint_accel_tau_s", 0.10f), 0.03f, 0.6f);
        decel_tau_s_ = clampf(params.value("sprint_decel_tau_s", 0.18f), 0.05f, 0.8f);
        max_duration_s_ = clampf(params.value("sprint_max_duration_s", 2.0f), 0.2f, 6.0f);
        cooldown_s_ = clampf(params.value("sprint_cooldown_s", 2.0f), 0.2f, 10.0f);
        vignette_on_sprint_ = params.value("vignette_on_sprint", true);
        vignette_intensity_override_ = params.value("vignette_intensity_override", -1.0f);
        deadzone_override_ = params.value("deadzone_override", -1.0f);

        // Reset state
        sprint_active_ = false;
        sprint_elapsed_s_ = 0.0f;
        cooldown_left_s_ = 0.0f;
        scalar_target_ = 1.0f;
        scalar_applied_ = 1.0f;
        vignette_applied_ = 0.0f;

        // Ensure we have a tag component to publish overlay values into (engine-safe no-op if already present)
        world.emplace_or_replace<CTag>(entity, CTag{});
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;

        // Read input-like live params from our binding entry (test harness drives these)
        bool left_click = false;                 // edge: pressed this frame
        bool left_click_held = false;            // held state (optional)
        float left_stick_mag = 0.0f;             // 0..1
        bool grounded = true;                    // fallback when physics contacts unavailable
        bool slide_active = false;               // true if any slide locomotion binding is present (and optionally active=true)
        float base_deadzone = 0.18f;             // default from scenes

        if (world.has<CMechanicBinding>(entity)) {
            auto& bind = world.get<CMechanicBinding>(entity);
            for (const auto& entry : bind.entries) {
                // Our overlay live input feed
                if (entry.id == id()) {
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if (j.is_object()) {
                        left_click      = j.value("left_stick_click", false);
                        left_click_held = j.value("left_stick_click_held", false);
                        left_stick_mag  = clampf(j.value("left_stick_mag", 0.0f), 0.0f, 1.0f);
                        grounded        = j.value("grounded", grounded);
                    }
                }
                // Detect if a slide locomotion module is present/active and read its deadzone
                if (starts_with(entry.id, "builtin.locomotion.slide.")) {
                    slide_active = true; // presence implies active by default
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if (j.is_object()) {
                        if (j.contains("active")) slide_active = j.value("active", slide_active);
                        base_deadzone = clampf(j.value("deadzone", base_deadzone), 0.0f, 1.0f);
                    }
                }
            }
        }

        // If available, prefer physics grounded from component (no contact data in public API; use kind=="dynamic" as hint only)
        if (world.has<CPhysics>(entity)) {
            // In a full build we'd inspect contact normals; here allow params-driven grounded to override.
            // TODO(#integrate): Plug real contact-grounding once exposed via World physics contacts.
        }

        // Decide effective deadzone
        float deadzone = base_deadzone;
        if (deadzone_override_ >= 0.0f) {
            deadzone = clampf(deadzone_override_, 0.18f, 0.25f);
        }

        // Sprint trigger: edge press while grounded, slide active, not cooling down
        if (left_click && grounded && slide_active && cooldown_left_s_ <= 0.0f) {
            sprint_active_ = true;
            sprint_elapsed_s_ = 0.0f;
            scalar_target_ = sprint_speed_multiplier_;
            publish_haptics_start();
        }

        // End conditions: stick dropped below deadzone, duration exceeded, lost ground, slide inactive
        if (sprint_active_) {
            sprint_elapsed_s_ += dt;
            bool end_now = false;
            if (sprint_elapsed_s_ >= max_duration_s_) end_now = true;
            if (!grounded) end_now = true;
            if (!slide_active) end_now = true;
            if (left_stick_mag < deadzone) end_now = true;
            if (!left_click_held && left_stick_mag < (deadzone + 0.02f)) {
                // If click not held, allow a soft release slightly above deadzone to be forgiving
                end_now = true;
            }
            if (end_now) {
                sprint_active_ = false;
                cooldown_left_s_ = cooldown_s_;
                scalar_target_ = 1.0f;
                publish_haptics_end();
                // TODO(#ui): Fire a short wrist cooldown chip via HUD bus when available.
            }
        }

        // Cooldown timer
        if (cooldown_left_s_ > 0.0f) {
            cooldown_left_s_ = std::max(0.0f, cooldown_left_s_ - dt);
        }

        // Smoothly approach target scalar using separate accel/decel taus
        const float tau = (scalar_target_ > scalar_applied_) ? accel_tau_s_ : decel_tau_s_;
        scalar_applied_ = exp_smooth(scalar_applied_, scalar_target_, dt, tau);

        // Vignette ramp (overlay publish via tag; real integration would talk to Comfort bus)
        const float vignette_target = (vignette_on_sprint_ && (sprint_active_ || scalar_target_ > 1.0f))
                                        ? resolve_vignette_target()
                                        : 0.0f;
        const float vignette_tau = (vignette_target > vignette_applied_) ? 0.10f : 0.14f;
        vignette_applied_ = exp_smooth(vignette_applied_, vignette_target, dt, vignette_tau);

        // Publish outputs into CTag as key=value pairs (engine-local convention for overlay shims)
        if (world.has<CTag>(entity)) {
            auto& tag = world.get<CTag>(entity);
            upsert_tag_kv(tag.tags, kLocomotionScalarKey, fmt_float(scalar_applied_, 3));
            upsert_tag_kv(tag.tags, kVignetteLevelKey,    fmt_float(vignette_applied_, 3));
            upsert_tag_kv(tag.tags, kSprintStateKey,      sprint_active_ ? "active" : (cooldown_left_s_ > 0.0f ? "cooldown" : "idle"));
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Clear our published keys to avoid stale overlays
        if (world.has<CTag>(entity)) {
            auto& tag = world.get<CTag>(entity);
            erase_tag_key(tag.tags, kLocomotionScalarKey);
            erase_tag_key(tag.tags, kVignetteLevelKey);
            erase_tag_key(tag.tags, kSprintStateKey);
        }
    }

    std::string id() const override { return "generated.ground_sprint_overlay"; }

private:
    // --- Config ---
    float sprint_speed_multiplier_{1.6f};
    float accel_tau_s_{0.10f};
    float decel_tau_s_{0.18f};
    float max_duration_s_{2.0f};
    float cooldown_s_{2.0f};
    bool  vignette_on_sprint_{true};
    float vignette_intensity_override_{-1.0f};
    float deadzone_override_{-1.0f};

    // --- State ---
    bool  sprint_active_{false};
    float sprint_elapsed_s_{0.0f};
    float cooldown_left_s_{0.0f};
    float scalar_target_{1.0f};
    float scalar_applied_{1.0f};
    float vignette_applied_{0.0f};

    // --- Publish keys (shared with downstream readers) ---
    static constexpr const char* kLocomotionScalarKey = "locomotion_scalar"; // consumed by slide blend in full build
    static constexpr const char* kVignetteLevelKey    = "comfort_vignette";  // consumed by Comfort bus in full build
    static constexpr const char* kSprintStateKey      = "sprint_state";      // debug/state UI

    // Helpers
    static bool starts_with(const std::string& s, const std::string& p) {
        return s.size() >= p.size() && std::equal(p.begin(), p.end(), s.begin());
    }
    static float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
    static float exp_smooth(float y, float target, float dt, float tau) {
        if (tau <= 0.0f) return target;
        const float a = 1.0f - std::exp(-dt / tau);
        return y + a * (target - y);
    }
    static std::string fmt_float(float v, int /*digits*/) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", v);
        return std::string(buf);
    }
    static void upsert_tag_kv(std::vector<std::string>& tags, const std::string& key, const std::string& val) {
        const std::string prefix = key + '=';
        auto it = std::find_if(tags.begin(), tags.end(), [&](const std::string& t){ return starts_with(t, prefix); });
        if (it == tags.end()) tags.push_back(prefix + val); else *it = prefix + val;
    }
    static void erase_tag_key(std::vector<std::string>& tags, const std::string& key) {
        const std::string prefix = key + '=';
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& t){ return starts_with(t, prefix); }), tags.end());
    }

    float resolve_vignette_target() const {
        if (vignette_intensity_override_ >= 0.0f) return clampf(vignette_intensity_override_, 0.0f, 1.0f);
        // TODO(#comfort): Query locomotion_options_ui ComfortBus for user vignette level.
        return 0.35f; // fallback comfort default
    }

    void publish_haptics_start() {
        if (ptv::haptics::has_pattern("sprint_start")) {
            const float user = ptv::haptics::get_user_intensity();
            ptv::haptics::publish("sprint_start", "left", 0.5f * user, std::nullopt);
        }
    }
    void publish_haptics_end() {
        if (ptv::haptics::has_pattern("sprint_end")) {
            const float user = ptv::haptics::get_user_intensity();
            ptv::haptics::publish("sprint_end", "left", 0.45f * user, std::nullopt);
        }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.ground_sprint_overlay", ptv::GroundSprintOverlay)
