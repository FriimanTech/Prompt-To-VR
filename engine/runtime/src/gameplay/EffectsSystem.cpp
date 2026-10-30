#include "EffectsSystem.h"

#include <algorithm>
#include <cmath>

namespace ptv {
namespace {

constexpr float kDecalLifeS = 14.0f;
constexpr float kDecalFadeS = 3.0f;
constexpr float kGravity = 9.81f;

bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 safe_normal(const Vec3& n, const Vec3& fallback) {
    const float l = glm::length(n);
    return (std::isfinite(l) && l > 1e-5f) ? n / l : fallback;
}

}  // namespace

void EffectsSystem::configure(const EffectsBudget& budget) {
    budget_ = budget;
    budget_.max_quads = std::max<std::size_t>(budget_.max_quads, 64);
    particles_.reserve(budget_.max_quads);
    tracers_.reserve(budget_.max_tracers);
    decals_.reserve(budget_.max_decals);
    lights_.reserve(std::max<std::size_t>(budget_.max_lights, 1));
}

void EffectsSystem::clear() {
    particles_.clear();
    tracers_.clear();
    decals_.clear();
    lights_.clear();
    frame_quads_.clear();
}

float EffectsSystem::rnd() {
    seed_ = seed_ * 1664525u + 1013904223u;
    return static_cast<float>(seed_ >> 8) / static_cast<float>(1u << 24);
}

Vec3 EffectsSystem::rnd_dir(const Vec3& around, float spread) {
    const Vec3 r(rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f, rnd() * 2.0f - 1.0f);
    return safe_normal(around + r * spread, around);
}

void EffectsSystem::add_particle(const Particle& p) {
    if (!finite(p.pos)) return;
    // The quad budget covers everything drawn; particles get most of it.
    const std::size_t cap = budget_.max_quads * 3 / 4;
    if (particles_.size() >= cap) {
        // Recycle the particle closest to death.
        auto it = std::min_element(particles_.begin(), particles_.end(),
                                   [](const Particle& a, const Particle& b) {
                                       return (a.max_life - a.life) < (b.max_life - b.life);
                                   });
        *it = p;
        return;
    }
    particles_.push_back(p);
}

void EffectsSystem::muzzle_flash(const Vec3& position, const Vec3& direction,
                                 const Vec4& color, float scale) {
    const Vec3 d = safe_normal(direction, Vec3(0, 0, -1));
    const float s = std::clamp(scale, 0.3f, 6.0f);
    Particle star{};
    star.pos = position + d * (0.025f * s);
    star.size = star.size_end = 0.09f * s;
    star.max_life = 0.05f;
    star.color = color;
    star.shape = FxShape::Star;
    star.spin = rnd() * 6.2831853f;
    add_particle(star);
    Particle core = star;
    core.shape = FxShape::Glow;
    core.size = 0.17f * s;
    core.size_end = 0.12f * s;
    core.max_life = 0.06f;
    add_particle(core);
    for (int i = 0; i < 3; ++i) {
        Particle sp{};
        sp.pos = position;
        sp.vel = rnd_dir(d, 0.25f) * (6.0f + 4.0f * rnd()) * s;
        sp.size = sp.size_end = 0.008f * s;
        sp.max_life = 0.06f + 0.04f * rnd();
        sp.color = color;
        sp.shape = FxShape::Streak;
        add_particle(sp);
    }
    light_flash(position + d * 0.05f, Vec3(color), 3.0f * s, 2.5f * s, 0.06f);
}

void EffectsSystem::tracer(const Vec3& from, const Vec3& to, float speed_mps,
                           const Vec4& color, float width_m, float length_m) {
    if (!finite(from) || !finite(to)) return;
    const Vec3 d = to - from;
    const float total = glm::length(d);
    if (total < 1e-3f) return;
    Tracer t{};
    t.from = from;
    t.dir = d / total;
    t.total = total;
    t.speed = std::clamp(speed_mps, 5.0f, 2000.0f);
    t.length = std::clamp(length_m, 0.02f, 20.0f);
    t.width = std::clamp(width_m, 0.001f, 0.5f);
    t.color = color;
    if (tracers_.size() >= budget_.max_tracers) {
        tracers_.erase(tracers_.begin());
    }
    tracers_.push_back(t);
}

void EffectsSystem::beam(const Vec3& from, const Vec3& to, float width_m, const Vec4& color) {
    if (!finite(from) || !finite(to)) return;
    FxQuad q{};
    q.shape = FxShape::Beam;
    q.a = from;
    q.b = to;
    q.size = std::clamp(width_m, 0.002f, 1.0f) * 3.0f;  // core + halo
    q.color = color;
    frame_quads_.push_back(q);
}

void EffectsSystem::glow(const Vec3& position, float size_m, const Vec4& color) {
    if (!finite(position)) return;
    FxQuad q{};
    q.shape = FxShape::Glow;
    q.a = position;
    q.size = std::clamp(size_m, 0.002f, 20.0f);
    q.color = color;
    frame_quads_.push_back(q);
}

void EffectsSystem::streak(const Vec3& from, const Vec3& to, float width_m,
                           const Vec4& color) {
    if (!finite(from) || !finite(to)) return;
    FxQuad q{};
    q.shape = FxShape::Streak;
    q.a = from;
    q.b = to;
    q.size = std::clamp(width_m, 0.001f, 1.0f);
    q.color = color;
    frame_quads_.push_back(q);
}

void EffectsSystem::impact(const Vec3& point, const Vec3& normal, ImpactStyle style,
                           const Vec4& color) {
    if (!finite(point)) return;
    const Vec3 n = safe_normal(normal, Vec3(0, 1, 0));
    switch (style) {
        case ImpactStyle::None:
            return;
        case ImpactStyle::Explosion:
            explosion(point, 1.5f, color);
            return;
        case ImpactStyle::Sparks: {
            for (int i = 0; i < 7; ++i) {
                Particle p{};
                p.pos = point + n * 0.005f;
                p.vel = rnd_dir(n, 0.9f) * (2.0f + 3.0f * rnd());
                p.size = p.size_end = 0.006f;
                p.max_life = 0.18f + 0.15f * rnd();
                p.color = Vec4(1.0f, 0.75f, 0.35f, 1.0f);
                p.shape = FxShape::Streak;
                p.gravity = 1.0f;
                add_particle(p);
            }
            Particle g{};
            g.pos = point + n * 0.01f;
            g.size = 0.07f;
            g.size_end = 0.03f;
            g.max_life = 0.07f;
            g.color = Vec4(1.0f, 0.8f, 0.5f, 1.0f);
            g.shape = FxShape::Glow;
            add_particle(g);
            Particle dust{};
            dust.pos = point + n * 0.02f;
            dust.vel = n * 0.3f;
            dust.size = 0.04f;
            dust.size_end = 0.12f;
            dust.max_life = 0.5f;
            dust.color = Vec4(0.55f, 0.52f, 0.48f, 0.45f);
            dust.shape = FxShape::Puff;
            add_particle(dust);
            return;
        }
        case ImpactStyle::Blood: {
            for (int i = 0; i < 6; ++i) {
                Particle p{};
                p.pos = point + n * 0.01f;
                p.vel = rnd_dir(n, 0.8f) * (0.6f + 1.2f * rnd());
                p.size = 0.03f + 0.02f * rnd();
                p.size_end = p.size * 2.2f;
                p.max_life = 0.35f + 0.25f * rnd();
                p.color = Vec4(color.r * 0.6f, color.g * 0.6f, color.b * 0.6f, 0.85f);
                p.shape = FxShape::Puff;
                p.gravity = 0.8f;
                p.drag = 2.0f;
                add_particle(p);
            }
            return;
        }
        case ImpactStyle::Energy: {
            Particle g{};
            g.pos = point + n * 0.01f;
            g.size = 0.22f;
            g.size_end = 0.05f;
            g.max_life = 0.16f;
            g.color = color;
            g.shape = FxShape::Glow;
            add_particle(g);
            for (int i = 0; i < 6; ++i) {
                Particle p{};
                p.pos = point + n * 0.01f;
                p.vel = rnd_dir(n, 1.0f) * (1.5f + 2.5f * rnd());
                p.size = p.size_end = 0.007f;
                p.max_life = 0.2f + 0.1f * rnd();
                p.color = color;
                p.shape = FxShape::Streak;
                p.drag = 3.0f;
                add_particle(p);
            }
            light_flash(point + n * 0.1f, Vec3(color), 1.5f, 1.5f, 0.1f);
            return;
        }
        case ImpactStyle::Dust: {
            for (int i = 0; i < 4; ++i) {
                Particle p{};
                p.pos = point + n * 0.02f;
                p.vel = rnd_dir(n, 0.6f) * (0.3f + 0.5f * rnd());
                p.size = 0.05f;
                p.size_end = 0.2f;
                p.max_life = 0.8f;
                p.color = Vec4(0.6f, 0.55f, 0.45f, 0.5f);
                p.shape = FxShape::Puff;
                p.drag = 1.5f;
                add_particle(p);
            }
            return;
        }
    }
}

void EffectsSystem::decal(const Vec3& point, const Vec3& normal, float size_m,
                          const Vec4& color) {
    if (!finite(point) || budget_.max_decals == 0) return;
    Decal d{};
    d.pos = point;
    d.normal = safe_normal(normal, Vec3(0, 1, 0));
    d.size = std::clamp(size_m, 0.005f, 2.0f);
    d.color = color;
    d.spin = rnd() * 6.2831853f;
    if (decals_.size() >= budget_.max_decals) decals_.erase(decals_.begin());
    decals_.push_back(d);
}

void EffectsSystem::explosion(const Vec3& center, float radius_m, const Vec4& color) {
    if (!finite(center)) return;
    const float r = std::clamp(radius_m, 0.1f, 30.0f);
    Particle core{};
    core.pos = center;
    core.size = 2.0f * r;
    core.size_end = 2.6f * r;
    core.max_life = 0.25f;
    core.color = Vec4(1.0f, 0.65f, 0.25f, 1.0f);
    core.shape = FxShape::Glow;
    add_particle(core);
    for (int i = 0; i < 10; ++i) {
        Particle s{};
        s.pos = center + rnd_dir(Vec3(0, 0.3f, 0), 1.0f) * (0.3f * r);
        s.vel = Vec3(0.0f, 0.8f + 0.6f * rnd(), 0.0f) + rnd_dir(Vec3(0), 1.0f) * 0.6f;
        s.size = 0.6f * r;
        s.size_end = 1.5f * r;
        s.max_life = 1.0f + 0.5f * rnd();
        s.color = Vec4(0.18f, 0.16f, 0.15f, 0.55f);
        s.shape = FxShape::Puff;
        s.drag = 1.2f;
        add_particle(s);
    }
    for (int i = 0; i < 14; ++i) {
        Particle sp{};
        sp.pos = center;
        sp.vel = rnd_dir(Vec3(0, 0.4f, 0), 1.0f) * (5.0f + 6.0f * rnd()) * std::sqrt(r);
        sp.size = sp.size_end = 0.02f;
        sp.max_life = 0.4f + 0.3f * rnd();
        sp.color = color;
        sp.shape = FxShape::Streak;
        sp.gravity = 1.0f;
        add_particle(sp);
    }
    light_flash(center, Vec3(1.0f, 0.6f, 0.25f), 8.0f, 6.0f * r, 0.3f);
}

void EffectsSystem::light_flash(const Vec3& position, const Vec3& color, float intensity,
                                float range_m, float duration_s) {
    if (!finite(position) || budget_.max_lights == 0) return;
    Light l{};
    l.pos = position;
    l.color = color;
    l.intensity = std::clamp(intensity, 0.0f, 50.0f);
    l.range = std::clamp(range_m, 0.2f, 60.0f);
    l.max_life = std::clamp(duration_s, 0.01f, 2.0f);
    if (lights_.size() >= budget_.max_lights) lights_.erase(lights_.begin());
    lights_.push_back(l);
}

void EffectsSystem::tick(float dt, Renderer& renderer) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    std::size_t quads = 0;
    // Particles.
    for (std::size_t i = 0; i < particles_.size();) {
        Particle& p = particles_[i];
        p.life += dt;
        if (p.life >= p.max_life) {
            p = particles_.back();
            particles_.pop_back();
            continue;
        }
        p.vel.y -= kGravity * p.gravity * dt;
        p.vel -= p.vel * std::min(1.0f, p.drag * dt);
        p.pos += p.vel * dt;
        const float t = p.life / p.max_life;
        FxQuad q{};
        q.shape = p.shape;
        q.color = p.color;
        q.color.a *= 1.0f - t * t;
        q.size = p.size + (p.size_end - p.size) * t;
        q.spin = p.spin;
        if (p.shape == FxShape::Streak) {
            const float sp = glm::length(p.vel);
            const Vec3 back = sp > 1e-4f ? p.vel / sp * std::clamp(sp * 0.02f, 0.02f, 0.5f)
                                         : Vec3(0.0f, 0.02f, 0.0f);
            q.a = p.pos - back;
            q.b = p.pos;
        } else {
            q.a = p.pos;
        }
        renderer.submit_fx(q);
        ++quads;
        ++i;
    }
    // Tracers.
    for (std::size_t i = 0; i < tracers_.size();) {
        Tracer& t = tracers_[i];
        t.travelled += t.speed * dt;
        const float head = std::min(t.travelled, t.total);
        const float tail = std::max(0.0f, t.travelled - t.length);
        if (tail >= t.total) {
            tracers_.erase(tracers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        FxQuad q{};
        q.shape = FxShape::Streak;
        q.a = t.from + t.dir * tail;
        q.b = t.from + t.dir * head;
        q.size = t.width;
        q.color = t.color;
        renderer.submit_fx(q);
        ++quads;
        ++i;
    }
    // Decals.
    for (std::size_t i = 0; i < decals_.size();) {
        Decal& d = decals_[i];
        d.age += dt;
        if (d.age >= kDecalLifeS) {
            decals_.erase(decals_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        FxQuad q{};
        q.shape = FxShape::Decal;
        q.a = d.pos;
        q.normal = d.normal;
        q.size = d.size;
        q.spin = d.spin;
        q.color = d.color;
        const float fade = std::clamp((kDecalLifeS - d.age) / kDecalFadeS, 0.0f, 1.0f);
        q.color.a *= fade;
        renderer.submit_fx(q);
        ++quads;
        ++i;
    }
    // This frame's beams / glows / streaks.
    for (const auto& q : frame_quads_) renderer.submit_fx(q);
    quads += frame_quads_.size();
    frame_quads_.clear();
    // Lights.
    for (std::size_t i = 0; i < lights_.size();) {
        lights_[i].life += dt;
        if (lights_[i].life >= lights_[i].max_life) {
            lights_.erase(lights_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    last_quads_ = quads;
}

void EffectsSystem::append_lights(std::vector<Renderer::PointLight>& lights) const {
    for (const auto& l : lights_) {
        Renderer::PointLight p{};
        p.position = l.pos;
        p.range = l.range;
        p.color = l.color;
        p.intensity = l.intensity * (1.0f - l.life / l.max_life);
        lights.push_back(p);
    }
}

}  // namespace ptv
