#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace ptv {

// Component tracked on a scene-level rescue controller entity.
struct CRescueState {
    int total{0};
    int rescued{0};
    // Index into civilians_ vector in the mechanic (for debug/telemetry via params); -1 means none.
    int following{-1};
};

// Small helpers local to this mechanic
inline float vec3_distance(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x; float dy = a.y - b.y; float dz = a.z - b.z;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

inline bool tag_list_contains(const std::vector<std::string>& tags, const std::string& wanted) {
    return std::find(tags.begin(), tags.end(), wanted) != tags.end();
}

// Query utilities confined to this mechanic
inline std::vector<std::uint32_t> find_entities_with_tag(World& w, const std::string& tag) {
    std::vector<std::uint32_t> out;
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        const auto& t = view.get<CTag>(e);
        if (tag_list_contains(t.tags, tag)) {
            out.push_back(static_cast<std::uint32_t>(e));
        }
    }
    return out;
}

inline std::optional<std::uint32_t> find_player_avatar(World& w) {
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    for (auto e : view) {
        const auto& t = view.get<CTag>(e);
        if (tag_list_contains(t.tags, "player_avatar")) {
            return static_cast<std::uint32_t>(e);
        }
    }
    return std::nullopt;
}

// Lightweight shield data tracked per-civilian during escort
struct ShieldData {
    float hp{0.0f};
    float max_hp{100.0f};
    float decay_per_s{0.0f};
    bool  active{false};
};

} // namespace ptv
