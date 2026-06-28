#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>

namespace ptv {

// Mission API forward decls (provided by mission_system task)
namespace mission_api {
void post_objective_progress(std::uint32_t objective_id, float progress);
bool mission_in_progress();
std::uint32_t current_objective_id();
float mission_elapsed();
int current_difficulty();
void set_score_delta(float v);
}

struct HazardConfig {
    // Common
    float radius{4.0f};        // meters; for zones (vents/yellow)
    float strength{1.0f};      // generic scalar
    float period_s{3.0f};      // generic period
    // Laser
    bool laser_reflectable{true};
    float laser_progress_rate{0.25f}; // per second toward objective
    // Vent
    float vent_pull_mps{2.0f};
    float vent_burst_duty{0.5f};
    // Debris
    float debris_interval_s{6.0f};
    float debris_telegraph_s{1.2f};
    float debris_hit_radius{0.6f};
    // Yellow zone
    float yellow_drain_multiplier{1.5f};
};

inline void parse_hazard_config(const nlohmann::json& p, HazardConfig& out) {
    out.radius = p.value("radius_m", out.radius);
    out.strength = p.value("strength", out.strength);
    out.period_s = p.value("period_s", out.period_s);
    out.laser_reflectable = p.value("laser_reflectable", out.laser_reflectable);
    out.laser_progress_rate = p.value("laser_progress_rate", out.laser_progress_rate);
    out.vent_pull_mps = p.value("vent_pull_mps", out.vent_pull_mps);
    out.vent_burst_duty = p.value("vent_burst_duty", out.vent_burst_duty);
    out.debris_interval_s = p.value("debris_interval_s", out.debris_interval_s);
    out.debris_telegraph_s = p.value("debris_telegraph_s", out.debris_telegraph_s);
    out.debris_hit_radius = p.value("debris_hit_radius_m", out.debris_hit_radius);
    out.yellow_drain_multiplier = p.value("yellow_drain_multiplier", out.yellow_drain_multiplier);
}

inline bool entity_has_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) return false;
    const auto& tags = w.get<CTag>(e).tags;
    for (const auto& t : tags) if (t == tag) return true;
    return false;
}

inline void ensure_tag(World& w, std::uint32_t e, const std::string& tag) {
    auto& ct = w.has<CTag>(e) ? w.get<CTag>(e) : (w.emplace_or_replace<CTag>(e, CTag{}), w.get<CTag>(e));
    for (const auto& t : ct.tags) if (t == tag) return;
    ct.tags.push_back(tag);
}

inline void remove_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) return;
    auto& ct = w.get<CTag>(e);
    auto& v = ct.tags;
    v.erase(std::remove(v.begin(), v.end(), tag), v.end());
}

inline std::optional<std::uint32_t> find_first_player(World& w) {
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    for (auto ent : view) {
        std::uint32_t e = static_cast<std::uint32_t>(ent);
        if (entity_has_tag(w, e, "player_avatar")) return e;
    }
    return std::nullopt;
}

// Distance helper
inline float dist(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x; float dy = a.y - b.y; float dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

// Clamp
inline float clamp01(float x){ return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

// Smooth step toward target
inline void approach(float& v, float target, float rate_per_s, float dt) {
    float delta = target - v;
    float step = rate_per_s * dt;
    if (std::fabs(delta) <= step) { v = target; return; }
    v += (delta > 0 ? step : -step);
}

// Debris path closest distance (segment point distance)
inline float segment_point_distance(const Vec3& a, const Vec3& b, const Vec3& p) {
    Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    Vec3 ap{p.x - a.x, p.y - a.y, p.z - a.z};
    float ab2 = ab.x*ab.x + ab.y*ab.y + ab.z*ab.z;
    float t = ab2 > 1e-6f ? (ab.x*ap.x + ab.y*ap.y + ab.z*ap.z)/ab2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    Vec3 c{a.x + ab.x*t, a.y + ab.y*t, a.z + ab.z*t};
    return dist(c, p);
}

} // namespace ptv
