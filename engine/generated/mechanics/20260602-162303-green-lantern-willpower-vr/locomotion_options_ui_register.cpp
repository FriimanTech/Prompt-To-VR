#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <optional>
#include <cmath>

// Forward declaration of HUD helper exported by dependency (no extra headers allowed)
namespace ptv { namespace hud_helpers {
    // Returns latest Settings page diff as JSON string, or empty if unchanged this frame.
    // Contract from dependency exports: ptv::hud_helpers::hud_get_settings_json(std::string&)
    void hud_get_settings_json(std::string& out_json);
}}

// Minimal forward decls to interact with Save/Persistence dependency without including headers
namespace ptv { namespace persistence_sections {
    // Store shallow-merged JSON blob under section key. If unavailable at runtime, no-op.
    void merge_shallow(const std::string& section, const std::string& json_blob);
    // Small digest for change detection. If unavailable, return empty.
    std::string tiny_digest(const std::string& json_blob);
}}

namespace ptv {

// Local bus published via CTag on the bound entity. Downstream mechanics read these tags.
// Keys coordinated with ground_sprint_overlay exports.
static constexpr const char* kTagComfortVignette = "comfort_vignette";        // value numeric string 0..1
static constexpr const char* kTagTurnMode        = "turn_mode";               // "snap" | "smooth"
static constexpr const char* kTagSnapAngle       = "snap_angle";              // degrees as number string
static constexpr const char* kTagSmoothDegPS     = "smooth_turn_speed_degps"; // deg/s as number string
static constexpr const char* kTagActiveLocomote  = "active_locomotion";       // teleport|slide_head|slide_controller

// Persistence section id for this mechanic
static constexpr const char* kPersistSection = "settings.locomotion";

struct LocoSettings {
    bool enable_teleport{true};
    bool enable_slide_head{false};
    bool enable_slide_controller{false};
    std::string turn_mode{"snap"}; // snap|smooth
    float snap_angle_deg{45.0f};    // 30/45/60
    float smooth_turn_speed_degps{180.0f}; // 90-240
    float vignette_intensity{0.0f}; // 0..1
    float height_offset_m{0.0f};    // -0.3..+0.3
};

static inline float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static nlohmann::json to_json(const LocoSettings& s) {
    nlohmann::json j;
    j["enable_teleport"] = s.enable_teleport;
    j["enable_slide_head"] = s.enable_slide_head;
    j["enable_slide_controller"] = s.enable_slide_controller;
    j["turn_mode"] = s.turn_mode;
    j["snap_angle_deg"] = s.snap_angle_deg;
    j["smooth_turn_speed_degps"] = s.smooth_turn_speed_degps;
    j["vignette_intensity"] = s.vignette_intensity;
    j["height_offset_m"] = s.height_offset_m;
    return j;
}

static LocoSettings from_json(const nlohmann::json& j, const LocoSettings& def) {
    LocoSettings s = def;
    if (!j.is_object()) return s;
    s.enable_teleport       = j.value("enable_teleport", s.enable_teleport);
    s.enable_slide_head     = j.value("enable_slide_head", s.enable_slide_head);
    s.enable_slide_controller = j.value("enable_slide_controller", s.enable_slide_controller);
    s.turn_mode             = j.value("turn_mode", s.turn_mode);
    s.snap_angle_deg        = j.value("snap_angle_deg", s.snap_angle_deg);
    s.smooth_turn_speed_degps = j.value("smooth_turn_speed_degps", s.smooth_turn_speed_degps);
    s.vignette_intensity    = clampf(j.value("vignette_intensity", s.vignette_intensity), 0.0f, 1.0f);
    s.height_offset_m       = clampf(j.value("height_offset_m", s.height_offset_m), -0.3f, 0.3f);
    // Enforce at least one locomotion active
    if (!s.enable_teleport && !s.enable_slide_head && !s.enable_slide_controller) {
        s.enable_teleport = true;
    }
    // Snap angle canonicalize to common values if close
    const float cand[3] = {30.0f, 45.0f, 60.0f};
    float best = s.snap_angle_deg, bestd = 1e9f;
    for (float c : cand) { float d = std::fabs(c - s.snap_angle_deg); if (d < bestd) { bestd = d; best = c; } }
    if (bestd < 3.0f) s.snap_angle_deg = best;
    s.smooth_turn_speed_degps = clampf(s.smooth_turn_speed_degps, 90.0f, 240.0f);
    if (s.turn_mode != "snap" && s.turn_mode != "smooth") s.turn_mode = "snap";
    return s;
}

// Safely parse params_json of a binding entry into a json object
static nlohmann::json safe_parse(const std::string& s) {
    return nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
}

// Ensure string tag key=value present (or updated) on CTag
static void tag_set_key_value(ptv::World& w, std::uint32_t e, const std::string& key, const std::string& value) {
    ptv::CTag tag;
    if (w.has<ptv::CTag>(e)) tag = w.get<ptv::CTag>(e);
    const std::string prefix = key + ":";
    bool found = false;
    for (auto& t : tag.tags) {
        if (t.rfind(prefix, 0) == 0) { t = prefix + value; found = true; break; }
    }
    if (!found) tag.tags.push_back(prefix + value);
    w.emplace_or_replace<ptv::CTag>(e, std::move(tag));
}

// Simple page registration stub: write a hint tag once so HUD can surface an entry chip if it wants.
static void ensure_locomotion_page_tag(ptv::World& w, std::uint32_t e) {
    tag_set_key_value(w, e, "ui_page", "settings.locomotion");
}

class LocomotionOptionsUI : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        entity_ = e;
        // Seed from existing mechanics where possible
        seed_from_current_bindings(w, e, settings_);
        // Apply any params override (e.g., from scene JSON)
        settings_ = from_json(params, settings_);
        // Reflect to world state
        ensure_locomotion_page_tag(w, e);
        publish_bus(w, e, settings_);
        apply_bindings(w, e, settings_);
        // Remember base Y to apply height offset additively
        if (w.has<CTransform>(e)) base_y_ = w.get<CTransform>(e).t.position.y;
        target_height_y_ = base_y_ + settings_.height_offset_m;
        current_height_y_ = target_height_y_;
        // Persist snapshot (best-effort, offline-ok)
        persist_if_available(settings_);
        posted_hint_ = false; // toast handled in companion file
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        // 1) Poll HUD for settings diffs
        std::string jstr;
        ptv::hud_helpers::hud_get_settings_json(jstr);
        if (!jstr.empty()) {
            auto j = safe_parse(jstr);
            if (j.is_object() && j.contains("page") && j["page"] == "settings.locomotion") {
                LocoSettings new_s = from_json(j.value("values", nlohmann::json::object()), settings_);
                // enforce exclusivity between slide profiles if both checked -> prefer last picked via explicit key if present
                if (j.contains("last_toggle")) {
                    std::string last = j.value("last_toggle", std::string(""));
                    if (last == "enable_slide_head" && new_s.enable_slide_head) new_s.enable_slide_controller = false;
                    if (last == "enable_slide_controller" && new_s.enable_slide_controller) new_s.enable_slide_head = false;
                }
                if (settings_changed(settings_, new_s)) {
                    settings_ = new_s;
                    publish_bus(w, e, settings_);
                    apply_bindings(w, e, settings_);
                    persist_if_available(settings_);
                }
            }
        }

        // 2) Smooth height offset application (comfort: no pops)
        if (w.has<CTransform>(e)) {
            float desired = base_y_ + clampf(settings_.height_offset_m, -0.3f, 0.3f);
            target_height_y_ = desired;
            // critically damped like lerp with small tau
            const float tau = 0.18f; // seconds
            float a = 1.0f - std::exp(-dt / std::max(1e-4f, tau));
            current_height_y_ = current_height_y_ + a * (target_height_y_ - current_height_y_);
            auto& tr = w.get<CTransform>(e).t;
            tr.position.y = current_height_y_;
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.locomotion_options_ui"; }

private:
    std::uint32_t entity_{0};
    LocoSettings settings_{};
    float base_y_{0.0f};
    float target_height_y_{0.0f};
    float current_height_y_{0.0f};
    bool posted_hint_{false};

    static bool settings_changed(const LocoSettings& a, const LocoSettings& b) {
        return a.enable_teleport != b.enable_teleport ||
               a.enable_slide_head != b.enable_slide_head ||
               a.enable_slide_controller != b.enable_slide_controller ||
               a.turn_mode != b.turn_mode ||
               std::fabs(a.snap_angle_deg - b.snap_angle_deg) > 0.5f ||
               std::fabs(a.smooth_turn_speed_degps - b.smooth_turn_speed_degps) > 0.5f ||
               std::fabs(a.vignette_intensity - b.vignette_intensity) > 0.01f ||
               std::fabs(a.height_offset_m - b.height_offset_m) > 0.005f;
    }

    static void seed_from_current_bindings(World& w, std::uint32_t e, LocoSettings& s) {
        if (!w.has<CMechanicBinding>(e)) return;
        const auto& mb = w.get<CMechanicBinding>(e);
        for (const auto& en : mb.entries) {
            const std::string& id = !en.id.empty() ? en.id : en.builtin_id;
            auto j = safe_parse(en.params_json);
            if (id == "builtin.locomotion.snap_turn") {
                if (j.is_object() && j.contains("degrees")) s.snap_angle_deg = j.value("degrees", s.snap_angle_deg);
            } else if (id == "builtin.locomotion.teleport") {
                s.enable_teleport = !j.value("disabled", false);
            } else if (id == "builtin.locomotion.slide.head_relative") {
                s.enable_slide_head = !j.value("disabled", false);
            } else if (id == "builtin.locomotion.slide.controller_relative") {
                s.enable_slide_controller = !j.value("disabled", false);
            }
        }
        // Turn mode inference: if snap_turn disabled in params, prefer smooth
        bool snap_enabled = true;
        if (w.has<CMechanicBinding>(e)) {
            const auto& mb2 = w.get<CMechanicBinding>(e);
            for (const auto& en : mb2.entries) {
                const std::string& id = !en.id.empty() ? en.id : en.builtin_id;
                if (id == "builtin.locomotion.snap_turn") {
                    auto j = safe_parse(en.params_json);
                    snap_enabled = !j.value("disabled", false);
                    if (j.is_object() && j.contains("degrees")) s.snap_angle_deg = j.value("degrees", s.snap_angle_deg);
                }
            }
        }
        s.turn_mode = snap_enabled ? std::string("snap") : std::string("smooth");
    }

    static void publish_bus(World& w, std::uint32_t e, const LocoSettings& s) {
        // Publish compact tags others can read quickly
        tag_set_key_value(w, e, kTagComfortVignette, std::to_string(clampf(s.vignette_intensity, 0.0f, 1.0f)));
        tag_set_key_value(w, e, kTagTurnMode, s.turn_mode);
        tag_set_key_value(w, e, kTagSnapAngle, std::to_string(s.snap_angle_deg));
        tag_set_key_value(w, e, kTagSmoothDegPS, std::to_string(s.smooth_turn_speed_degps));
        // Active locomotion chip: prefer a single active slide profile if any, otherwise teleport
        std::string active = s.enable_slide_head ? "slide_head" : (s.enable_slide_controller ? "slide_controller" : (s.enable_teleport ? "teleport" : "teleport"));
        tag_set_key_value(w, e, kTagActiveLocomote, active);
    }

    static void apply_bindings(World& w, std::uint32_t e, const LocoSettings& s) {
        if (!w.has<CMechanicBinding>(e)) return;
        auto mb = w.get<CMechanicBinding>(e); // copy -> we'll write back
        for (auto& en : mb.entries) {
            std::string id = !en.id.empty() ? en.id : en.builtin_id;
            auto j = safe_parse(en.params_json);
            if (!j.is_object()) j = nlohmann::json::object();
            if (id == "builtin.locomotion.teleport") {
                j["disabled"] = !s.enable_teleport;
            } else if (id == "builtin.locomotion.slide.head_relative") {
                // Only one sliding profile active at a time
                j["disabled"] = !s.enable_slide_head || s.enable_slide_controller;
            } else if (id == "builtin.locomotion.slide.controller_relative") {
                j["disabled"] = !s.enable_slide_controller || s.enable_slide_head;
            } else if (id == "builtin.locomotion.snap_turn") {
                j["degrees"] = s.snap_angle_deg;
                // Disable snap turn when smooth requested; enable otherwise
                j["disabled"] = (s.turn_mode == "smooth");
            }
            en.params_json = j.dump();
        }
        // Write back edited binding
        w.emplace_or_replace<CMechanicBinding>(e, std::move(mb));
    }

    static void persist_if_available(const LocoSettings& s) {
        // Best-effort save via dependency service; safe even if not linked (stubbed by linker to no-op or ODR resolved)
        try {
            const auto blob = to_json(s).dump();
            ptv::persistence_sections::merge_shallow(kPersistSection, blob);
            (void)ptv::persistence_sections::tiny_digest(blob);
        } catch (...) {
            // Offline-ok; ignore
        }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.locomotion_options_ui", ptv::LocomotionOptionsUI)
