#pragma once

// ctx.vfx: gameplay visual effects, owned and pooled by the engine.
//
// Muzzle flashes, tracers, energy bolts, continuous beams, impact sparks /
// blood / energy splashes, bullet-hole decals, explosions and short light
// flashes. Generated code asks for an effect by what it IS; the engine owns
// the particles, their lifetimes and their caps (the scene's performance
// block), and draws them through FxRenderer with no textures. Calls beyond
// the caps recycle the oldest effect rather than allocate, so an automatic
// weapon cannot run a Quest out of frame time.

#include <cstdint>

#include "ptv/math/Math.h"

namespace ptv {

enum class ImpactStyle : int {
    Sparks = 0,
    Blood = 1,
    Energy = 2,
    Dust = 3,
    Explosion = 4,
    None = 5,
};

class IEffects {
public:
    virtual ~IEffects() = default;

    /** A flash at a muzzle: ``position`` and ``direction`` are the muzzle
     *  socket's origin and +Z. ``scale`` 1 is a pistol, 2 a rifle, 3+ a
     *  launcher. Also emits a brief light. */
    virtual void muzzle_flash(const Vec3& position, const Vec3& direction,
                              const Vec4& color, float scale) = 0;

    /** A streak that travels from ``from`` to ``to`` at ``speed_mps`` (a
     *  tracer for bullets, ``length_m`` long and ``width_m`` wide). */
    virtual void tracer(const Vec3& from, const Vec3& to, float speed_mps,
                        const Vec4& color, float width_m, float length_m) = 0;

    /** A continuous beam for THIS FRAME only: call every frame it is on. */
    virtual void beam(const Vec3& from, const Vec3& to, float width_m,
                      const Vec4& color) = 0;

    /** What a shot looks like where it lands. */
    virtual void impact(const Vec3& point, const Vec3& normal, ImpactStyle style,
                        const Vec4& color) = 0;

    /** A mark left on a surface (bullet hole, scorch). Fades after a while. */
    virtual void decal(const Vec3& point, const Vec3& normal, float size_m,
                       const Vec4& color) = 0;

    /** A fireball of ``radius_m``, with light. */
    virtual void explosion(const Vec3& center, float radius_m, const Vec4& color) = 0;

    /** A short point light (muzzle flash, explosion). */
    virtual void light_flash(const Vec3& position, const Vec3& color, float intensity,
                             float range_m, float duration_s) = 0;

    /** A glowing billboard for THIS FRAME only (an energy bolt in flight, a
     *  charge-up glow). */
    virtual void glow(const Vec3& position, float size_m, const Vec4& color) = 0;

    /** A streak for THIS FRAME only (a moving bolt's trail, a bowstring). */
    virtual void streak(const Vec3& from, const Vec3& to, float width_m,
                        const Vec4& color) = 0;
};

}  // namespace ptv
