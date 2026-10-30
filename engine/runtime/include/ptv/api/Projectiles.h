#pragma once

// ctx.projectiles: objects that fly with travel time, owned by the engine.
//
// A projectile is NOT an ECS entity and has no physics body: the engine keeps
// a fixed pool, integrates gravity and drag (sub-stepped for fast shots),
// and tests each step's path with a swept ray or sphere -- continuous
// collision by construction, so nothing tunnels through a wall at 900 m/s.
// On a hit it applies damage through ICombat (instigator = the shooter),
// splash damage with a line-of-sight check, an impulse to dynamic bodies,
// the impact effect, then its on-hit behaviour (despawn, stick, bounce,
// explode). Its visual is drawn from a template mesh oriented along its
// velocity (an arrow, a rocket), or as a tracer / bolt; render-only, so a
// rifle's worth of shots costs no physics.
//
// builtin.weapon and builtin.bow launch through this. Generated code may
// launch too (a custom weapon), and reads hits through ``hits_since``.

#include <cstdint>

#include "ptv/api/Effects.h"
#include "ptv/math/Math.h"

namespace ptv {

enum class ProjectileOnHit : int {
    Despawn = 0,
    /** Stays where it hit for a while (arrows), riding a moving target. */
    Stick = 1,
    /** Bounces off static geometry, losing energy; explodes or despawns at
     *  the end of its life (grenades). */
    Bounce = 2,
    Explode = 3,
};

enum class ProjectileVisual : int {
    None = 0,
    Tracer = 1,
    Bolt = 2,
    /** ``visual_template``'s mesh, oriented by its ``nose`` socket. */
    Mesh = 3,
};

struct ProjectileDesc {
    Vec3 origin{0.0f};
    Vec3 velocity{0.0f, 0.0f, -100.0f};
    float gravity_scale{1.0f};
    /** Linear drag coefficient (1/s): ``v -= v * drag * dt``. */
    float drag{0.0f};
    /** 0 = a ray; > 0 = a swept sphere. */
    float radius_m{0.0f};
    float lifetime_s{3.0f};
    float max_distance_m{1000.0f};
    /** Who fired it (the player or an NPC) and with what. Both are passed
     *  through by its hit test, along with whatever the shooter holds. */
    std::uint32_t shooter{0xFFFFFFFFu};
    std::uint32_t weapon{0xFFFFFFFFu};
    float damage{0.0f};
    float splash_radius_m{0.0f};
    float splash_damage{0.0f};
    ProjectileOnHit on_hit{ProjectileOnHit::Despawn};
    ProjectileVisual visual{ProjectileVisual::Tracer};
    /** A hidden template entity whose mesh is the projectile's look. */
    std::uint32_t visual_template{0xFFFFFFFFu};
    Vec4 color{1.0f, 0.85f, 0.5f, 1.0f};
    float visual_width_m{0.01f};
    float visual_length_m{0.35f};
    ImpactStyle impact{ImpactStyle::Sparks};
    /** Stick only: how long a stuck projectile stays. */
    float stick_s{6.0f};
};

struct ProjectileHit {
    std::uint64_t seq{0};
    std::uint32_t shooter{0xFFFFFFFFu};
    std::uint32_t weapon{0xFFFFFFFFu};
    /** The entity struck (health owner when the collider was its gear), or
     *  invalid for level geometry. */
    std::uint32_t target{0xFFFFFFFFu};
    Vec3 point{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    float damage{0.0f};
    bool killed{false};
};

class IProjectiles {
public:
    virtual ~IProjectiles() = default;

    /** Launch one projectile. Returns a handle (>0), or 0 when the pool is
     *  disabled. A full pool recycles its oldest projectile. */
    virtual std::uint32_t launch(const ProjectileDesc& desc) = 0;
    virtual int live_count() const = 0;
    /** Monotonic hit counter; compare with your last seen value. */
    virtual std::uint64_t hit_seq() const = 0;
    /** Up to ``max_out`` hits after ``after_seq``, oldest first. */
    virtual int hits_since(std::uint64_t after_seq, ProjectileHit* out,
                           int max_out) const = 0;
    /** Fill ``out`` with a designed projectile's flight and look (the
     *  scene's ``projectiles`` table, by id). ``out->velocity`` is set to
     *  (0, 0, -speed): scale your direction by its length. The caller still
     *  sets origin, direction, shooter, weapon and damage. False when the
     *  scene has no such projectile. */
    virtual bool preset(const char* projectile_id, ProjectileDesc* out) const {
        (void)projectile_id;
        (void)out;
        return false;
    }
};

}  // namespace ptv
