#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cmath>

namespace ptv {

// Role detected from mesh/tags
enum class EnemyRole : std::uint8_t {
    Ground,
    Drone,
    Turret,
    Brute
};

struct GroundData {
    enum class Phase : std::uint8_t { SeekCover, BurstFire, Flank, BreakAttempt };
    Phase phase{Phase::SeekCover};
    float fire_cooldown{0.0f};
    float burst_timer{0.0f};
    int   burst_shots_left{0};
};

struct DroneData {
    enum class Phase : std::uint8_t { BandHold, StrafeLeft, StrafeRight, JukeUp, JukeDown };
    Phase phase{Phase::BandHold};
    float band_min{8.0f};
    float band_max{16.0f};
    float strafe_t{0.0f};
};

struct TurretData {
    enum class Phase : std::uint8_t { AcquireLane, AimTelegraph, Snipe, BlockedReacquire, BurstWhenFlanked };
    Phase phase{Phase::AcquireLane};
    float telegraph_t{0.0f};
    float fire_cooldown{0.0f};
};

struct BruteData {
    enum class Phase : std::uint8_t { RoarTelegraph, Rush, MeleeWindup, Recover };
    Phase phase{Phase::RoarTelegraph};
    float aura_radius{4.0f};
    float rush_speed{8.0f};
    float windup_t{0.0f};
};

struct PerEntityState {
    EnemyRole role{EnemyRole::Ground};
    float t{0.0f};
    float difficulty{1.0f};
    // Union-ish payloads
    GroundData ground{};
    DroneData  drone{};
    TurretData turret{};
    BruteData  brute{};

    // Cached random-ish seed from entity id for phase offsets
    float seed{0.0f};
};

// Helpers
inline std::optional<std::uint32_t> find_player_entity(World& w) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        const auto& tag = view.get<CTag>(e);
        for (const auto& s : tag.tags) {
            if (s == "player_avatar") {
                return static_cast<std::uint32_t>(e);
            }
        }
    }
    return std::nullopt;
}

inline ptv::Vec3 get_entity_position(World& w, std::uint32_t e) {
    if (w.has<CTransform>(e)) {
        return w.get<CTransform>(e).t.position;
    }
    return Vec3{0.0f, 0.0f, 0.0f};
}

inline void write_move_tag(World& w, std::uint32_t e, const ptv::Vec3& p) {
    std::string t = "ai:move:" + std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z);
    if (w.has<CTag>(e)) {
        auto& ct = w.get<CTag>(e);
        // replace existing move tag
        for (auto& s : ct.tags) {
            if (s.rfind("ai:move:", 0) == 0) { s = t; return; }
        }
        ct.tags.push_back(t);
    } else {
        CTag c{}; c.tags.push_back(t); w.emplace_or_replace<CTag>(e, std::move(c));
    }
}

inline void write_aim_tag(World& w, std::uint32_t e, const ptv::Vec3& dir) {
    std::string t = "ai:aim:" + std::to_string(dir.x) + "," + std::to_string(dir.y) + "," + std::to_string(dir.z);
    if (w.has<CTag>(e)) {
        auto& ct = w.get<CTag>(e);
        for (auto& s : ct.tags) {
            if (s.rfind("ai:aim:", 0) == 0) { s = t; return; }
        }
        ct.tags.push_back(t);
    } else {
        CTag c{}; c.tags.push_back(t); w.emplace_or_replace<CTag>(e, std::move(c));
    }
}

inline void write_fire_tag(World& w, std::uint32_t e, const std::string& mode, float rate_hz) {
    std::string t = std::string("ai:fire:") + mode + ":rate=" + std::to_string(rate_hz);
    if (w.has<CTag>(e)) {
        auto& ct = w.get<CTag>(e);
        // allow multiple fire modes; just append/update first matching prefix
        for (auto& s : ct.tags) {
            if (s.rfind("ai:fire:", 0) == 0) { s = t; return; }
        }
        ct.tags.push_back(t);
    } else {
        CTag c{}; c.tags.push_back(t); w.emplace_or_replace<CTag>(e, std::move(c));
    }
}

inline void write_state_flag(World& w, std::uint32_t e, const std::string& flag) {
    if (w.has<CTag>(e)) {
        auto& ct = w.get<CTag>(e);
        for (auto& s : ct.tags) { if (s == flag) return; }
        ct.tags.push_back(flag);
    } else {
        CTag c{}; c.tags.push_back(flag); w.emplace_or_replace<CTag>(e, std::move(c));
    }
}

inline void write_aura_tag(World& w, std::uint32_t e, float radius_m, float drain_mult) {
    std::string t = "ai:aura:yellow:r=" + std::to_string(radius_m) + ":mult=" + std::to_string(drain_mult);
    if (w.has<CTag>(e)) {
        auto& ct = w.get<CTag>(e);
        for (auto& s : ct.tags) { if (s.rfind("ai:aura:yellow:", 0) == 0) { s = t; return; } }
        ct.tags.push_back(t);
    } else {
        CTag c{}; c.tags.push_back(t); w.emplace_or_replace<CTag>(e, std::move(c));
    }
}

inline bool scene_has_tag(World& w, const std::string& needle) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        const auto& tag = view.get<CTag>(e);
        for (const auto& s : tag.tags) if (s == needle) return true;
    }
    return false;
}

inline bool entity_has_tag(World& w, std::uint32_t e, const std::string& t) {
    if (!w.has<CTag>(e)) return false;
    const auto& ct = w.get<CTag>(e);
    for (const auto& s : ct.tags) if (s == t) return true;
    return false;
}

// Math helpers
inline Vec3 normalize_safe(const Vec3& v) {
    float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (len < 1e-4f) return Vec3{0,0,0};
    return Vec3{v.x/len, v.y/len, v.z/len};
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

inline float dot(const Vec3& a, const Vec3& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

} // namespace ptv
