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

struct TurretParams { float snipe_rate_hz{0.8f}; float telegraph_time{0.8f}; };

inline void ai_tick_turret(World& w, std::uint32_t e, PerEntityState& s, const TurretParams& tp, float dt) {
    s.t += dt;
    auto playerE = find_player_entity(w);
    if (!playerE) return;

    Vec3 me = get_entity_position(w, e);
    Vec3 pl = get_entity_position(w, *playerE);
    Vec3 toP = pl - me;
    Vec3 toPdir = normalize_safe(toP);

    bool barrier_present = scene_has_tag(w, "barrier");

    switch (s.turret.phase) {
        case TurretData::Phase::AcquireLane: {
            write_aim_tag(w, e, toPdir);
            write_state_flag(w, e, "vfx:laser_line_on"); // advertise lane
            // If barrier in scene, consider lane blocked sometimes -> reacquire or flank burst
            if (barrier_present && std::fmod(s.t + s.seed, 2.0f) < dt) {
                s.turret.phase = TurretData::Phase::BlockedReacquire;
            } else {
                s.turret.phase = TurretData::Phase::AimTelegraph;
                s.turret.telegraph_t = 0.0f;
            }
        } break;
        case TurretData::Phase::AimTelegraph: {
            write_aim_tag(w, e, toPdir);
            write_state_flag(w, e, "vfx:sniper_glint");
            s.turret.telegraph_t += dt;
            if (s.turret.telegraph_t >= tp.telegraph_time) {
                s.turret.phase = TurretData::Phase::Snipe;
                s.turret.fire_cooldown = 0.0f;
            }
        } break;
        case TurretData::Phase::Snipe: {
            write_aim_tag(w, e, toPdir);
            s.turret.fire_cooldown -= dt;
            if (s.turret.fire_cooldown <= 0.0f) {
                write_fire_tag(w, e, "snipe", tp.snipe_rate_hz * s.difficulty);
                s.turret.fire_cooldown = 1.0f / (tp.snipe_rate_hz * s.difficulty);
            }
            // Occasionally check for block/flank to vary behaviour
            if (barrier_present && std::fmod(s.t + s.seed, 3.0f) < dt) {
                s.turret.phase = TurretData::Phase::BlockedReacquire;
            } else if (std::fmod(s.t + s.seed*2.0f, 5.0f) < dt) {
                s.turret.phase = TurretData::Phase::AcquireLane;
            }
        } break;
        case TurretData::Phase::BlockedReacquire: {
            write_state_flag(w, e, "vfx:laser_line_search");
            // Random small sweep using seed
            float ang = std::sin(s.t*0.7f + s.seed*3.1f) * 0.6f;
            Vec3 up{0,1,0};
            Vec3 side = normalize_safe(cross(up, toPdir));
            Vec3 sweepDir = normalize_safe(toPdir * std::cos(ang) + side * std::sin(ang));
            write_aim_tag(w, e, sweepDir);
            if (std::fmod(s.t + s.seed, 1.2f) < dt) {
                // If feels flanked (simulated), switch to burst for counterplay window
                s.turret.phase = TurretData::Phase::BurstWhenFlanked;
                s.turret.fire_cooldown = 0.0f;
            }
        } break;
        case TurretData::Phase::BurstWhenFlanked: {
            // Short close-range suppression burst
            write_aim_tag(w, e, toPdir);
            s.turret.fire_cooldown -= dt;
            if (s.turret.fire_cooldown <= 0.0f) {
                write_fire_tag(w, e, "burst", 6.0f * s.difficulty);
                s.turret.fire_cooldown = 0.18f;
            }
            if (std::fmod(s.t + s.seed, 1.0f) < dt) {
                s.turret.phase = TurretData::Phase::AcquireLane;
            }
        } break;
    }
}

} // namespace ptv
