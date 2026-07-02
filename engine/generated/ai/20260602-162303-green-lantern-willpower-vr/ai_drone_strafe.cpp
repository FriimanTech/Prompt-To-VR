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

struct DroneParams { float strafe_speed{6.0f}; float juke_period{0.9f}; float altitude_bias{1.5f}; };

inline void ai_tick_drone(World& w, std::uint32_t e, PerEntityState& s, const DroneParams& dp, float dt) {
    s.t += dt;
    auto playerE = find_player_entity(w);
    if (!playerE) return;

    Vec3 me = get_entity_position(w, e);
    Vec3 pl = get_entity_position(w, *playerE);
    Vec3 toP = pl - me;
    Vec3 toPdir = normalize_safe(toP);

    bool dome_present = scene_has_tag(w, "dome_shield");

    // Maintain distance band; move inward/outward
    float d = std::sqrt(toP.x*toP.x + toP.y*toP.y + toP.z*toP.z);
    float target_d = std::clamp(d, s.drone.band_min, s.drone.band_max);
    float rad_err = d - 0.5f*(s.drone.band_min + s.drone.band_max);

    Vec3 up{0,1,0};
    Vec3 side = normalize_safe(cross(up, toPdir));

    // Pick strafe direction with periodic jukes
    float wave = std::sin((s.seed*1.7f + s.t) * (2.0f*3.1415926f/dp.juke_period));
    Vec3 strafe = side * (wave >= 0 ? 1.0f : -1.0f);

    // Altitude bias relative to player
    float alt = (pl.y + dp.altitude_bias) - me.y;

    // Avoid dome interior by biasing outward if dome present
    Vec3 radial = toPdir * ((rad_err > 0) ? 1.0f : -1.0f);
    if (dome_present && rad_err < 0) radial = toPdir; // push outward

    Vec3 vel = radial * 2.0f + strafe * dp.strafe_speed + Vec3{0, alt * 1.5f, 0};
    Vec3 target = me + vel * dt;

    write_move_tag(w, e, target);
    write_aim_tag(w, e, toPdir);

    // Light burst fire while strafing
    float rate = 4.0f * s.difficulty * (0.6f + 0.4f*std::abs(wave));
    write_fire_tag(w, e, "burst", rate);
}

} // namespace ptv
