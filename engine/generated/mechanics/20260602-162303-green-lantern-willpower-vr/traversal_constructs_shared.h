#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include <optional>
#include <cmath>

namespace ptv {

// Local math helpers (no engine additions)
inline Vec3 quat_rotate(const Quat& q, const Vec3& v) {
    // q * v * conj(q)
    const float qw = q.w, qx = q.x, qy = q.y, qz = q.z;
    // t = 2 * cross(q.xyz, v)
    Vec3 t{ 2.0f * (qy * v.z - qz * v.y),
            2.0f * (qz * v.x - qx * v.z),
            2.0f * (qx * v.y - qy * v.x) };
    // v' = v + qw * t + cross(q.xyz, t)
    Vec3 qxyz{ qx, qy, qz };
    Vec3 c{ qxyz.y * t.z - qxyz.z * t.y,
            qxyz.z * t.x - qxyz.x * t.z,
            qxyz.x * t.y - qxyz.y * t.x };
    return Vec3{ v.x + qw * t.x + c.x,
                 v.y + qw * t.y + c.y,
                 v.z + qw * t.z + c.z };
}

inline Quat look_rotation_from_forward_up(const Vec3& fwd, const Vec3& up_hint) {
    // Build orthonormal basis with forward (z) and up hint; then to quaternion.
    Vec3 z = fwd; float zl = std::sqrt(z.x*z.x+z.y*z.y+z.z*z.z); if (zl>1e-6f){ z.x/=zl; z.y/=zl; z.z/=zl; }
    // x = normalize(cross(up, z))
    Vec3 x{ up_hint.y*z.z - up_hint.z*z.y,
            up_hint.z*z.x - up_hint.x*z.z,
            up_hint.x*z.y - up_hint.y*z.x };
    float xl = std::sqrt(x.x*x.x+x.y*x.y+x.z*x.z); if (xl>1e-6f){ x.x/=xl; x.y/=xl; x.z/=xl; } else { x = Vec3{1,0,0}; }
    // y = cross(z, x)
    Vec3 y{ z.y*x.z - z.z*x.y,
            z.z*x.x - z.x*x.z,
            z.x*x.y - z.y*x.x };
    // Rotation matrix columns (x,y,z)
    float m00=x.x, m01=y.x, m02=z.x;
    float m10=x.y, m11=y.y, m12=z.y;
    float m20=x.z, m21=y.z, m22=z.z;
    float tr = m00 + m11 + m22;
    Quat q{1,0,0,0};
    if (tr > 0.0f) {
        float S = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * S;
        q.x = (m21 - m12) / S;
        q.y = (m02 - m20) / S;
        q.z = (m10 - m01) / S;
    } else if ((m00 > m11) && (m00 > m22)) {
        float S = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / S;
        q.x = 0.25f * S;
        q.y = (m01 + m10) / S;
        q.z = (m02 + m20) / S;
    } else if (m11 > m22) {
        float S = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / S;
        q.x = (m01 + m10) / S;
        q.y = 0.25f * S;
        q.z = (m12 + m21) / S;
    } else {
        float S = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / S;
        q.x = (m02 + m20) / S;
        q.y = (m12 + m21) / S;
        q.z = 0.25f * S;
    }
    return q;
}

struct Spawned {
    entt::entity e{entt::null};
    float ttl_s{0.0f};
};

struct LadderPreviewState {
    std::vector<Spawned> steps;
    float spacing_m{0.35f};
    int count{5};
    float max_length_m{3.0f};
    bool confirmed{false};
};

struct RamRideState {
    entt::entity ram{entt::null};
    bool riding{false};
    float speed_mps{4.0f};
    bool stabilize{true};
    bool cancel_on_collision{true};
    float life_s{30.0f};
};

enum class TravMode : std::uint8_t { None=0, Ladder, Ram };

struct TraversalState {
    TravMode mode{TravMode::None};
    LadderPreviewState ladder;
    RamRideState ram;
    nlohmann::json last_params; // latest parsed params for this frame
};

// Haptics publish prefixes from dependency (copied through ledger at runtime by including the string values here when used)
inline void publish_haptics(World& w, entt::entity where, const std::string& pattern_id, const std::string& hand, float amp, float dur_s) {
    // Safe: attach a CTag with a publish string. Do not overwrite existing tags blindly; append.
    CTag tag;
    if (w.valid(static_cast<std::uint32_t>(where)) && w.has<CTag>(static_cast<std::uint32_t>(where))) {
        tag = w.get<CTag>(static_cast<std::uint32_t>(where));
    }
    nlohmann::json msg{{"pattern_id", pattern_id},{"hand", hand},{"intensity_scale", amp},{"duration_s", dur_s}};
    tag.tags.push_back(std::string("haptics.publish:") + msg.dump());
    if (w.valid(static_cast<std::uint32_t>(where))) {
        w.emplace_or_replace<CTag>(static_cast<std::uint32_t>(where), tag);
    }
}

// Util: destroy spawned list safely
inline void cleanup_spawned(World& w, std::vector<Spawned>& list) {
    auto& reg = w.registry();
    for (auto& s : list) {
        if (s.e != entt::null && reg.valid(s.e)) reg.destroy(s.e);
    }
    list.clear();
}

// Factory helpers (implemented in .cpps)
void ladders_preview_update(World& w, std::uint32_t hand_entity, TraversalState& st, const ptv::Vec3& base_point, const ptv::Vec3& surface_normal, const ptv::Quat& orient, float dt);
void ladders_confirm_finalize(World& w, TraversalState& st);

void ram_update(World& w, std::uint32_t hand_entity, TraversalState& st, const ptv::Vec3& start_pos, const ptv::Quat& hand_rot, float dt, bool cancel);

} // namespace ptv
