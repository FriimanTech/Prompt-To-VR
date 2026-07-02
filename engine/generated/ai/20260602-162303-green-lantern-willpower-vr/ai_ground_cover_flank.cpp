#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <cmath>
#include "enemy_ai_shared.h"

namespace ptv {

struct GroundParams { float move_speed{3.5f}; float burst_rate_hz{6.0f}; int burst_size{3}; float flank_distance{4.0f}; };

inline void ai_tick_ground(World& w, std::uint32_t e, PerEntityState& s, const GroundParams& gp, float dt) {
    s.t += dt;
    auto playerE = find_player_entity(w);
    if (!playerE) return;

    Vec3 me = get_entity_position(w, e);
    Vec3 pl = get_entity_position(w, *playerE);
    Vec3 toP = pl - me;
    Vec3 toPdir = normalize_safe(toP);

    // Barrier or dome present nudges behaviour toward flank
    bool barrier_present = scene_has_tag(w, "barrier") || scene_has_tag(w, "dome_shield");
    bool caged = entity_has_tag(w, e, "caged");

    if (caged) {
        s.ground.phase = GroundData::Phase::BreakAttempt;
    }

    switch (s.ground.phase) {
        case GroundData::Phase::SeekCover: {
            // Move to a position offset from player line; simulate cover by lateral offset
            Vec3 up{0,1,0};
            Vec3 side = normalize_safe(cross(up, toPdir));
            float sideSign = ((static_cast<int>(s.seed*997) % 2) ? 1.0f : -1.0f);
            Vec3 target = pl - toPdir * 9.0f + side * (2.0f * sideSign);
            write_move_tag(w, e, target);
            write_aim_tag(w, e, toPdir);
            // Transition when roughly in band
            if (std::abs(dot(pl - target, Vec3{1,0,0})) < 9.0f) {
                s.ground.phase = barrier_present ? GroundData::Phase::Flank : GroundData::Phase::BurstFire;
                s.ground.burst_shots_left = gp.burst_size;
                s.ground.burst_timer = 0.0f;
                s.ground.fire_cooldown = 0.0f;
            }
        } break;
        case GroundData::Phase::BurstFire: {
            write_move_tag(w, e, me + toPdir * 0.0f); // hold
            write_aim_tag(w, e, toPdir);
            s.ground.fire_cooldown -= dt;
            if (s.ground.fire_cooldown <= 0.0f && s.ground.burst_shots_left > 0) {
                write_fire_tag(w, e, "burst", gp.burst_rate_hz * s.difficulty);
                s.ground.burst_shots_left--;
                s.ground.fire_cooldown = 1.0f / (gp.burst_rate_hz * s.difficulty);
            }
            s.ground.burst_timer += dt;
            if (s.ground.burst_timer > 0.6f || s.ground.burst_shots_left == 0) {
                s.ground.burst_timer = 0.0f;
                s.ground.burst_shots_left = gp.burst_size;
                // Occasionally reposition
                s.ground.phase = barrier_present ? GroundData::Phase::Flank : GroundData::Phase::SeekCover;
            }
        } break;
        case GroundData::Phase::Flank: {
            // Perpendicular move around barriers
            Vec3 up{0,1,0};
            Vec3 side = normalize_safe(cross(up, toPdir));
            float weave = std::sin(s.t * (0.6f + 0.3f*s.seed));
            Vec3 target = me + side * (gp.flank_distance * (weave >= 0 ? 1.0f : -1.0f));
            write_move_tag(w, e, target);
            write_aim_tag(w, e, toPdir);
            // After some time, resume burst
            if (s.t - std::floor(s.t) < dt) { // roughly once per second edge
                s.ground.phase = GroundData::Phase::BurstFire;
            }
        } break;
        case GroundData::Phase::BreakAttempt: {
            // If caged, aim at nearest bar center: here just aim at player and fire continuously to "break"
            write_move_tag(w, e, me); // struggle in place
            write_aim_tag(w, e, toPdir);
            write_state_flag(w, e, "anim:struggle");
            write_fire_tag(w, e, "continuous", 3.0f * s.difficulty);
            if (!caged) {
                s.ground.phase = GroundData::Phase::SeekCover;
            }
        } break;
    }
}

} // namespace ptv
