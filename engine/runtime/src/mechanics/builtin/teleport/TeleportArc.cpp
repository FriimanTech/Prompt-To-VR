#include "TeleportArc.h"

namespace ptv {
namespace teleport {

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
            const Vec3 seg = pos - prev;
            const float seg_len = glm::length(seg);
            PhysicsRayHit hit;
            if (seg_len > 1e-5f &&
                physics->raycast(prev, seg, seg_len, hit) && hit.hit) {
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
