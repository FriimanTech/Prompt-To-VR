#include "TeleportArc.h"

#include <algorithm>
#include <cmath>

namespace ptv {
namespace teleport {
namespace {

bool ignored(const ArcParams& p, std::uint32_t entity) {
    return std::find(p.ignore_entities.begin(), p.ignore_entities.end(),
                     entity) != p.ignore_entities.end();
}

/** Raycast that skips the ignored bodies by re-casting just past them. */
bool cast_segment(IPhysicsAccess* physics, const ArcParams& p,
                  const Vec3& from, const Vec3& to, PhysicsRayHit& out) {
    Vec3 start = from;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const Vec3 seg = to - start;
        const float len = glm::length(seg);
        if (len < 1e-5f) return false;
        PhysicsRayHit hit;
        if (!physics->raycast(start, seg, len, hit) || !hit.hit) return false;
        if (!ignored(p, hit.entity)) {
            out = hit;
            return true;
        }
        start = hit.point + (seg / len) * 0.02f;
    }
    return false;
}

}  // namespace

float distance_for_pitch(float pitch_rad, float min_distance_m,
                         float max_distance_m) {
    const float lo = std::max(0.3f, min_distance_m);
    const float hi = std::max(lo + 0.1f, max_distance_m);
    const float t = std::clamp((pitch_rad - kMinAimPitch) /
                                   (kMaxAimPitch - kMinAimPitch),
                               0.0f, 1.0f);
    // Ease in: fine control close to the player, where most teleports land,
    // and a faster reach once the controller is raised toward the horizon.
    const float eased = t * t * (3.0f - 2.0f * t) * 0.5f + t * t * 0.5f;
    return lo + (hi - lo) * eased;
}

float reachable_distance(float pitch_rad, float distance_m, float height_m) {
    if (pitch_rad >= -1e-3f) return distance_m;
    const float line = height_m / std::tan(-pitch_rad);
    return std::max(0.3f, std::min(distance_m, 0.85f * line));
}

float launch_speed_for(float pitch_rad, float distance_m, float height_m,
                       float gravity_mps2) {
    const float c = std::cos(pitch_rad);
    const float denom = 2.0f * c * c *
                        (distance_m * std::tan(pitch_rad) + height_m);
    if (!(denom > 1e-4f) || c < 1e-3f) return 0.5f;
    const float v2 = gravity_mps2 * distance_m * distance_m / denom;
    return std::sqrt(std::max(0.25f, v2));
}

void solve_arc(const Vec3& origin, const Vec3& forward, const ArcParams& p,
               IPhysicsAccess* physics, ArcResult& out) {
    out.points.clear();
    out.valid = false;

    // ~33ms integration step over at most ~8 seconds of flight. Coarse on
    // purpose: this is a preview, and every extra sample is another draw call
    // for the arc ribbon.
    constexpr float kStep = 0.033f;
    constexpr int kMaxSteps = 240;

    Vec3 pos = origin;
    Vec3 vel = forward * p.launch_speed_mps;
    out.points.push_back(pos);

    for (int i = 0; i < kMaxSteps; ++i) {
        const Vec3 prev = pos;
        vel.y -= p.gravity_mps2 * kStep;
        pos += vel * kStep;
        out.points.push_back(pos);

        // Real geometry first, so platforms and terrain are landable.
        if (physics != nullptr) {
            PhysicsRayHit hit;
            if (cast_segment(physics, p, prev, pos, hit)) {
                out.points.back() = hit.point;
                Vec3 horiz = hit.point - origin;
                horiz.y = 0.0f;
                if (glm::length(horiz) > p.max_horizontal_m) return;
                if (hit.normal.y < p.min_ground_normal_y) return;  // a wall
                out.target = hit.point;
                out.valid = true;
                return;
            }
        }

        // Otherwise fall through to the flat floor plane.
        if (pos.y <= p.floor_y && prev.y > p.floor_y) {
            const float t = (prev.y - p.floor_y) / (prev.y - pos.y);
            Vec3 hit = prev + (pos - prev) * t;
            hit.y = p.floor_y;
            out.points.back() = hit;
            Vec3 horiz = hit - origin;
            horiz.y = 0.0f;
            if (glm::length(horiz) > p.max_horizontal_m) return;
            out.target = hit;
            out.valid = true;
            return;
        }

        // Stop tracing once the arc is clearly out of range.
        if (glm::length(Vec3(pos.x - origin.x, 0.0f, pos.z - origin.z)) >
            p.max_horizontal_m + 1.0f) {
            break;
        }
    }
}

}  // namespace teleport
}  // namespace ptv
