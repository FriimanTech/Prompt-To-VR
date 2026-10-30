#pragma once

// The engine side of ctx.vfx (ptv/api/Effects.h): fixed pools of particles,
// tracers, decals and flash lights, simulated on the CPU and handed to
// FxRenderer as quads each frame. Every pool is capped (the scene's
// performance block); a full pool recycles its oldest element, so effects
// never allocate during play and never grow without bound.

#include <cstdint>
#include <vector>

#include "ptv/api/Effects.h"
#include "ptv/render/FxRenderer.h"
#include "ptv/render/Renderer.h"

namespace ptv {

struct EffectsBudget {
    std::size_t max_quads{1024};
    std::size_t max_decals{48};
    std::size_t max_lights{2};
    std::size_t max_tracers{64};
};

class EffectsSystem final : public IEffects {
public:
    void configure(const EffectsBudget& budget);
    void clear();

    // IEffects
    void muzzle_flash(const Vec3& position, const Vec3& direction, const Vec4& color,
                      float scale) override;
    void tracer(const Vec3& from, const Vec3& to, float speed_mps, const Vec4& color,
                float width_m, float length_m) override;
    void beam(const Vec3& from, const Vec3& to, float width_m, const Vec4& color) override;
    void impact(const Vec3& point, const Vec3& normal, ImpactStyle style,
                const Vec4& color) override;
    void decal(const Vec3& point, const Vec3& normal, float size_m,
               const Vec4& color) override;
    void explosion(const Vec3& center, float radius_m, const Vec4& color) override;
    void light_flash(const Vec3& position, const Vec3& color, float intensity,
                     float range_m, float duration_s) override;
    void glow(const Vec3& position, float size_m, const Vec4& color) override;
    void streak(const Vec3& from, const Vec3& to, float width_m, const Vec4& color) override;

    /** Advance every effect by ``dt`` and submit this frame's quads. */
    void tick(float dt, Renderer& renderer);
    /** Add this frame's flash lights to the renderer's point lights. */
    void append_lights(std::vector<Renderer::PointLight>& lights) const;

    std::size_t live_particles() const { return particles_.size(); }
    std::size_t live_quads() const { return last_quads_; }

private:
    struct Particle {
        Vec3 pos{0.0f};
        Vec3 vel{0.0f};
        float life{0.0f};
        float max_life{0.1f};
        float size{0.05f};
        float size_end{0.05f};
        Vec4 color{1.0f};
        FxShape shape{FxShape::Glow};
        float gravity{0.0f};
        float drag{0.0f};
        float spin{0.0f};
    };
    struct Tracer {
        Vec3 from{0.0f};
        Vec3 dir{0.0f, 0.0f, -1.0f};
        float total{0.0f};
        float travelled{0.0f};
        float speed{300.0f};
        float length{0.4f};
        float width{0.01f};
        Vec4 color{1.0f};
    };
    struct Decal {
        Vec3 pos{0.0f};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        float size{0.03f};
        Vec4 color{0.1f, 0.1f, 0.1f, 0.9f};
        float age{0.0f};
        float spin{0.0f};
    };
    struct Light {
        Vec3 pos{0.0f};
        Vec3 color{1.0f};
        float intensity{1.0f};
        float range{3.0f};
        float life{0.0f};
        float max_life{0.05f};
    };

    void add_particle(const Particle& p);
    float rnd();                       // 0..1
    Vec3 rnd_dir(const Vec3& around, float spread);

    EffectsBudget budget_{};
    std::vector<Particle> particles_;
    std::vector<Tracer> tracers_;
    std::vector<Decal> decals_;
    std::vector<Light> lights_;
    std::vector<FxQuad> frame_quads_;  // beams / glows / streaks for this frame
    std::size_t last_quads_{0};
    std::uint32_t seed_{0x9E3779B9u};
};

}  // namespace ptv
