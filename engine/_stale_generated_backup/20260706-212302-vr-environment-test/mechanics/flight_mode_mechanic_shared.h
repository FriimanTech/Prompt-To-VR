#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <optional>

namespace ptv {

// Lightweight runtime flight state written by this mechanic.
struct CFlightState {
    bool  active{false};
    bool  boost{false};
    float speed_mps{0.0f};
    float fov_ease01{0.0f};
};

// Helper: clamp
inline float ptv_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Helper: safe json get
inline float j_get_f(const nlohmann::json& j, const char* k, float defv) {
    if (j.is_object() && j.contains(k) && j.at(k).is_number()) return j.at(k).get<float>();
    return defv;
}
inline bool j_get_b(const nlohmann::json& j, const char* k, bool defv) {
    if (j.is_object() && j.contains(k) && j.at(k).is_boolean()) return j.at(k).get<bool>();
    return defv;
}
inline std::string j_get_s(const nlohmann::json& j, const char* k, const std::string& defv) {
    if (j.is_object() && j.contains(k) && j.at(k).is_string()) return j.at(k).get<std::string>();
    return defv;
}

// Fetch this mechanic's live params blob (driven by the binding each frame).
// Returns object json or empty object.
inline nlohmann::json ptv_flight_fetch_self_params(World& w, std::uint32_t e, const char* self_mech_id) {
    if (!w.has<CMechanicBinding>(e)) return nlohmann::json::object();
    auto& mb = w.get<CMechanicBinding>(e);
    for (auto& it : mb.entries) {
        if (it.id == self_mech_id || it.builtin_id == self_mech_id) {
            nlohmann::json j = nlohmann::json::parse(it.params_json, nullptr, /*allow_exceptions=*/false);
            return j.is_object() ? j : nlohmann::json::object();
        }
    }
    return nlohmann::json::object();
}

// Writes back computed visualization scalars into our own params_json so downstream VFX/camera can read them.
inline void ptv_flight_publish_vfx(World& w, std::uint32_t e, const char* self_mech_id, float speed_scalar01, float fov_ease01) {
    if (!w.has<CMechanicBinding>(e)) return;
    auto& mb = w.get<CMechanicBinding>(e);
    for (auto& it : mb.entries) {
        if (it.id == self_mech_id || it.builtin_id == self_mech_id) {
            nlohmann::json j = nlohmann::json::parse(it.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) j = nlohmann::json::object();
            j["flight_trail_speed_scalar01"] = ptv_clamp(speed_scalar01, 0.0f, 1.0f);
            j["camera_fov_ease01"] = ptv_clamp(fov_ease01, 0.0f, 1.0f);
            it.params_json = j.dump();
            break;
        }
    }
}

// Attempt to ping hover mechanic by toggling a flag in its params_json. If not present, also drop a request tag.
inline void ptv_request_hover_now(World& w, std::uint32_t e) {
    bool flagged = false;
    if (w.has<CMechanicBinding>(e)) {
        auto& mb = w.get<CMechanicBinding>(e);
        for (auto& it : mb.entries) {
            if (it.id == std::string("generated.hover_mode_mechanic") || it.builtin_id == std::string("generated.hover_mode_mechanic")) {
                nlohmann::json j = nlohmann::json::parse(it.params_json, nullptr, /*allow_exceptions=*/false);
                if (!j.is_object()) j = nlohmann::json::object();
                j["engage_hover_now"] = true; // convention used by hover_mode_mechanic
                it.params_json = j.dump();
                flagged = true;
                break;
            }
        }
    }
    if (!flagged) {
        // Fallback convention: add a tag others can watch.
        CTag t;
        if (w.has<CTag>(e)) t = w.get<CTag>(e);
        auto& tags = t.tags;
        if (std::find(tags.begin(), tags.end(), std::string("hover_request")) == tags.end()) tags.push_back("hover_request");
        w.emplace_or_replace<CTag>(e, std::move(t));
    }
}

class FlightModeMechanic : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return std::string(kId); }

    // Expose id to helpers
    static constexpr const char* kId = "generated.flight_mode_mechanic";

private:
    // Tunables (loaded from params once; input states are read per-frame from binding params_json)
    float base_speed_mps_   {5.0f};
    float boost_max_mps_    {12.0f};
    float accel_tau_s_      {0.18f};
    float decel_tau_s_      {0.22f};
    float altitude_rate_mps_{2.0f};
    bool  right_stick_altitude_{true};
    float bank_max_deg_     {20.0f};
    bool  horizon_lock_     {true};
    float drain_per_s_      {1.0f};
    float boost_extra_drain_{3.0f};

    // State
    bool  active_{false};
    bool  boost_{false};
    bool  braking_{false};
    float brake_timer_{0.0f};
    ptv::Vec3 velocity_{0.0f, 0.0f, 0.0f};
};

} // namespace ptv
