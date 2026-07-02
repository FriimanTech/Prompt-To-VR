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

struct BruteParams { float rush_speed{9.0f}; float aura_radius{5.0f}; float aura_mult{1.35f}; float windup_time{0.7f}; };

inline void ai_tick_brute(World& w, std::uint32_t e, PerEntityState& s, const BruteParams& bp, float dt) {
    s.t += dt;
    auto playerE = find_player_entity(w);
    if (!playerE) return;

    Vec3 me = get_entity_position(w, e);
    Vec3 pl = get_entity_position(w, *playerE);
    Vec3 toP = pl - me;
    Vec3 toPdir = normalize_safe(toP);

    // Publish suppression aura (for promoted.ring_charge_and_recharge to consume)
    write_aura_tag(w, e, bp.aura_radius, bp.aura_mult);

    switch (s.brute.phase) {
        case BruteData::Phase::RoarTelegraph: {
            write_state_flag(w, e, "vfx:brute_roar");
            s.brute.windup_t += dt;
            if (s.brute.windup_t >= 0.6f) {
                s.brute.phase = BruteData::Phase::Rush;
            }
        } break;
        case BruteData::Phase::Rush: {
            Vec3 target = me + toPdir * (bp.rush_speed * s.difficulty * dt);
            write_move_tag(w, e, target);
            write_aim_tag(w, e, toPdir);
            // Close enough -> melee windup
            float dist = std::sqrt(toP.x*toP.x + toP.y*toP.y + toP.z*toP.z);
            if (dist < 2.2f) {
                s.brute.phase = BruteData::Phase::MeleeWindup;
                s.brute.windup_t = 0.0f;
                write_state_flag(w, e, "vfx:melee_windup");
            }
        } break;
        case BruteData::Phase::MeleeWindup: {
            s.brute.windup_t += dt;
            write_aim_tag(w, e, toPdir);
            if (s.brute.windup_t >= 0.45f) {
                // Heavy swing telegraphed -> emit melee tag (engine hitbox hook reads it)
                write_fire_tag(w, e, "melee_heavy", 1.0f);
                s.brute.phase = BruteData::Phase::Recover;
                s.brute.windup_t = 0.0f;
            }
        } break;
        case BruteData::Phase::Recover: {
            // Brief recovery then re-engage
            s.brute.windup_t += dt;
            if (s.brute.windup_t >= 0.6f) {
                s.brute.phase = BruteData::Phase::Rush;
                s.brute.windup_t = 0.0f;
            }
        } break;
    }
}

} // namespace ptv
