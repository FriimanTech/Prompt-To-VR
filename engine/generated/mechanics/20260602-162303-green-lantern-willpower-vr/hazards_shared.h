#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <vector>
#include <cmath>

namespace ptv {

// Lightweight publisher: push a CTag protocol string "prefix{json}"
inline void hazards_push_tag(World& w, std::uint32_t e, const std::string& prefix, const nlohmann::json& j) {
    CTag tag{};
    if (w.has<CTag>(e)) tag = w.get<CTag>(e);
    tag.tags.emplace_back(prefix + j.dump());
    w.emplace_or_replace<CTag>(e, std::move(tag));
}

inline std::optional<ptv::Vec3> j_vec3(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return std::nullopt;
    auto& o = j.at(key);
    if (!o.is_object() || !o.contains("x") || !o.contains("y") || !o.contains("z")) return std::nullopt;
    return ptv::Vec3{ o.at("x").get<float>(), o.at("y").get<float>(), o.at("z").get<float>() };
}

inline ptv::Vec3 safe_normalize(const ptv::Vec3& v) {
    float len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-6f) return ptv::Vec3{0,0,0};
    float inv = 1.0f / std::sqrt(len2);
    return ptv::Vec3{v.x*inv, v.y*inv, v.z*inv};
}

inline ptv::Vec3 reflect(const ptv::Vec3& d, const ptv::Vec3& n) {
    // r = d - 2(d·n)n ; assume d is incident direction (unit) and n is unit normal
    float dn = d.x*n.x + d.y*n.y + d.z*n.z;
    return ptv::Vec3{ d.x - 2.0f*dn*n.x, d.y - 2.0f*dn*n.y, d.z - 2.0f*dn*n.z };
}

struct LaserState {
    ptv::Vec3 dir{0,0,1};
    float max_range_m{50.0f};
    bool powered_once{false};
    float pulse_timer{0.0f};
    float pulse_period_s{0.5f};
    std::string power_node_id; // optional, empty if none
};

struct VentState {
    float period_s{3.0f};
    float on_s{1.2f};
    float suction_strength{6.0f}; // m/s impulse suggestion
    float t{0.0f};
    bool fail_respawn{true};
};

struct DebrisSpawnerState {
    float interval_s{5.0f};
    float warn_s{1.0f};
    float speed_mps{18.0f};
    float ttl_s{6.0f};
    float t{0.0f};
    int sequence{0};
};

struct YellowZoneState {
    float drain_multiplier{1.5f}; // +50%
    float tether_break_add{0.2f};
    bool active_last{false};
};

enum class HazardKind { Unknown, Laser, Vent, DebrisSpawner, YellowZone };

struct HazardRuntime {
    HazardKind kind{HazardKind::Unknown};
    LaserState laser;
    VentState vent;
    DebrisSpawnerState debris;
    YellowZoneState yellow;
};

inline HazardKind infer_hazard_kind(World& w, std::uint32_t e, const nlohmann::json& params) {
    // Explicit param has priority
    if (params.is_object() && params.contains("kind")) {
        auto k = params.at("kind").get<std::string>();
        if (k == "laser") return HazardKind::Laser;
        if (k == "vent") return HazardKind::Vent;
        if (k == "debris") return HazardKind::DebrisSpawner;
        if (k == "yellow_zone") return HazardKind::YellowZone;
    }
    // Fallback: use presence of scene tags on this entity
    if (w.has<CTag>(e)) {
        const auto& tags = w.get<CTag>(e).tags;
        auto has = [&](const char* s){ return std::find(tags.begin(), tags.end(), std::string(s)) != tags.end(); };
        if (has("laser")) return HazardKind::Laser;
        if (has("vent")) return HazardKind::Vent;
        if (has("yellow_zone") || has("suppression_zone")) return HazardKind::YellowZone;
        if (has("hazard") || has("trigger")) return HazardKind::DebrisSpawner; // generic fallback
    }
    return HazardKind::Unknown;
}

// Mission System tag (from dependency exports): "objective.progress:"
static constexpr const char* kObjectiveProgress = "objective.progress:";
// HUD indicator lightweight protocol
static constexpr const char* kHudIndicator = "hud.indicator:"; // {id:"suppression",color:"amber",state:"on|off",ttl}
// Ring charge/recharge protocol (request form; consumed by promoted.ring_charge_and_recharge)
static constexpr const char* kRingDrainMultiplier = "ring.drain.multiplier:"; // {mult, ttl}
// Control suppression (for tether break chance tweaks)
static constexpr const char* kControlSuppression = "control.suppression:"; // {tether_break_add, ttl}
// Hazard forces (for locomotion/hover aids to optionally consume)
static constexpr const char* kHazardForce = "hazard.force:"; // {dir{x,y,z}, strength, ttl}
// Spawner requests (seen elsewhere in project: portal_travel uses spawner.request)
static constexpr const char* kSpawnerRequest = "spawner.request:"; // {id, pos{x,y,z}, dir{x,y,z}, speed, ttl}

} // namespace ptv
