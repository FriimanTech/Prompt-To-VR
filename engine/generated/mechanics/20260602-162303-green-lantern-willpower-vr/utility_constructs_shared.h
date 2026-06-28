#pragma once
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>

namespace utility_internal {

struct ScannerState {
    entt::entity drone{entt::null};
    float scan_interval{2.0f};
    float range{18.0f};
    float t_accum{0.0f};
    bool enabled{true};
};

struct GrabberState {
    entt::entity hand{entt::null};
    float pull_force{35.0f};
    entt::entity latched{entt::null};
};

struct RepairState {
    float rate{0.25f};
    entt::entity current{entt::null};
    std::unordered_map<entt::entity, float> progress; // 0..1
};

struct BeaconState {
    entt::entity beacon{entt::null};
    float intensity{1.0f};
    float head_rot_t{0.0f};
};

struct BubbleState {
    struct BubbleComp { float hp{50.0f}; float time_left{8.0f}; };
    // No persistent per-mechanic state required beyond defaults
    float default_hp{50.0f};
    float default_duration{8.0f};
};

// Safe tag helpers
inline bool has_tag(const ptv::World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<ptv::CTag>(e)) return false;
    const auto& tags = w.get<ptv::CTag>(e).tags;
    for (const auto& t : tags) if (t == tag) return true;
    return false;
}
inline void add_tag(ptv::World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<ptv::CTag>(e)) { w.emplace_or_replace<ptv::CTag>(e, ptv::CTag{}); }
    auto& tags = w.get<ptv::CTag>(e).tags;
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
}
inline void remove_tag(ptv::World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<ptv::CTag>(e)) return;
    auto& tags = w.get<ptv::CTag>(e).tags;
    tags.erase(std::remove(tags.begin(), tags.end(), tag), tags.end());
}

// JSON helpers (read from binding params)
inline bool jflag(const nlohmann::json& j, const char* k, bool defv=false) {
    if (!j.is_object()) return defv;
    if (!j.contains(k)) return defv;
    return j.at(k).get<bool>();
}
inline float jfloat(const nlohmann::json& j, const char* k, float defv=0.f) {
    if (!j.is_object()) return defv;
    if (!j.contains(k)) return defv;
    return j.at(k).get<float>();
}
inline ptv::Vec3 jvec3(const nlohmann::json& j, const char* k, const ptv::Vec3& defv={0,0,0}) {
    if (!j.is_object()) return defv;
    if (!j.contains(k)) return defv;
    const auto& a = j.at(k);
    if (a.is_array() && a.size()==3) {
        return ptv::Vec3{a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
    }
    return defv;
}

// Simple forward cone test using positions only (no physics queries available here)
inline bool in_forward_cone(const ptv::Transform& origin, const ptv::Vec3& fwd, const ptv::Vec3& target, float maxDist, float cosHalfAngle) {
    const ptv::Vec3 to = target - origin.position;
    const float d2 = dot(to, to);
    if (d2 > maxDist*maxDist) return false;
    const float d = std::sqrt(std::max(0.0f, d2));
    if (d <= 1e-4f) return true;
    const ptv::Vec3 dir = to / d;
    return dot(dir, fwd) >= cosHalfAngle;
}

} // namespace utility_internal
