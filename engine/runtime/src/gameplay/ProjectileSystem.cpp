#include "ProjectileSystem.h"

#include "CombatRules.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/quaternion.hpp>

#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kGravity = 9.81f;
constexpr float kBounceRestitution = 0.45f;

bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Quat rotation_between(const Vec3& from, const Vec3& to) {
    const Vec3 a = glm::normalize(from);
    const Vec3 b = glm::normalize(to);
    const float d = glm::dot(a, b);
    if (d > 0.99999f) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.99999f) {
        Vec3 axis = glm::cross(Vec3(1, 0, 0), a);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(Vec3(0, 1, 0), a);
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    const Vec3 axis = glm::cross(a, b);
    return glm::normalize(Quat(1.0f + d, axis.x, axis.y, axis.z));
}

void set_hidden(World& world, std::uint32_t e, bool hidden) {
    auto& tag = world.registry().get_or_emplace<CTag>(static_cast<entt::entity>(e));
    auto it = std::find(tag.tags.begin(), tag.tags.end(), "template");
    if (hidden && it == tag.tags.end()) tag.tags.push_back("template");
    if (!hidden && it != tag.tags.end()) tag.tags.erase(it);
}

}  // namespace

void ProjectileSystem::configure(int max_live, int max_mesh_visuals) {
    pool_.assign(static_cast<std::size_t>(std::clamp(max_live, 1, 2048)), Live{});
    max_mesh_visuals_ = std::clamp(max_mesh_visuals, 0, 256);
}

void ProjectileSystem::clear() {
    for (auto& p : pool_) p = Live{};
    if (svc_.world != nullptr) {
        for (const auto& v : visuals_) {
            if (v.entity != kNone && svc_.world->valid(v.entity)) {
                svc_.world->registry().destroy(static_cast<entt::entity>(v.entity));
            }
        }
    }
    visuals_.clear();
}

bool ProjectileSystem::preset(const char* projectile_id, ProjectileDesc* out) const {
    if (projectile_id == nullptr || out == nullptr) return false;
    for (const auto& p : presets_) {
        if (p.id != projectile_id) continue;
        *out = p.desc;
        if (!p.template_name.empty() && svc_.world != nullptr) {
            const auto e = svc_.world->find(p.template_name);
            if (e != entt::null) {
                out->visual_template = static_cast<std::uint32_t>(e);
                out->visual = ProjectileVisual::Mesh;
            }
        }
        return true;
    }
    return false;
}

int ProjectileSystem::live_count() const {
    int n = 0;
    for (const auto& p : pool_) n += p.state != State::Free ? 1 : 0;
    return n;
}

int ProjectileSystem::hits_since(std::uint64_t after_seq, ProjectileHit* out,
                                 int max_out) const {
    if (out == nullptr || max_out <= 0 || hit_seq_ <= after_seq) return 0;
    const std::uint64_t cap = hits_.size();
    std::uint64_t first = std::max<std::uint64_t>(after_seq + 1,
                                                  hit_seq_ > cap ? hit_seq_ - cap + 1 : 1);
    int n = 0;
    for (std::uint64_t s = first; s <= hit_seq_ && n < max_out; ++s) {
        out[n++] = hits_[static_cast<std::size_t>((s - 1) % cap)];
    }
    return n;
}

std::uint32_t ProjectileSystem::launch(const ProjectileDesc& desc) {
    if (pool_.empty() || !finite(desc.origin) || !finite(desc.velocity)) return 0;
    // A free slot, else recycle the oldest projectile (stuck ones first).
    Live* slot = nullptr;
    for (auto& p : pool_) {
        if (p.state == State::Free) {
            slot = &p;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &*std::max_element(pool_.begin(), pool_.end(), [](const Live& a, const Live& b) {
            const float ka = a.age + (a.state != State::Flying ? 1000.0f : 0.0f);
            const float kb = b.age + (b.state != State::Flying ? 1000.0f : 0.0f);
            return ka < kb;
        });
        release_visual(*slot);
    }
    Live p{};
    p.state = State::Flying;
    p.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    p.desc = desc;
    p.desc.lifetime_s = std::clamp(desc.lifetime_s, 0.02f, 60.0f);
    p.pos = desc.origin;
    p.vel = desc.velocity;
    if (desc.visual == ProjectileVisual::Mesh && desc.visual_template != kNone) {
        p.visual = acquire_visual(desc.visual_template);
    }
    *slot = p;
    return slot->id;
}

int ProjectileSystem::build_ignore(const Live& p, std::uint32_t* out, int cap) const {
    int n = 0;
    auto add = [&](std::uint32_t e) {
        if (e != kNone && n < cap) out[n++] = e;
    };
    add(p.desc.shooter);
    add(p.desc.weapon);
    if (svc_.hands != nullptr) {
        const std::uint32_t player = svc_.hands->player_entity();
        if (p.desc.shooter == player || p.desc.shooter == kNone) {
            add(player);
            for (int h = 0; h < 2; ++h) {
                add(svc_.hands->hand_entity(h));
                add(svc_.hands->held_entity(h));
            }
        }
    }
    if (p.visual >= 0) add(visuals_[static_cast<std::size_t>(p.visual)].entity);
    return n;
}

std::uint32_t ProjectileSystem::health_owner(std::uint32_t entity) const {
    return combat_rules::health_owner(*svc_.world, entity);
}

bool ProjectileSystem::may_damage(std::uint32_t shooter, std::uint32_t target) const {
    return combat_rules::may_damage(svc_.combat, shooter, target);
}

void ProjectileSystem::record_hit(const Live& p, std::uint32_t target, const Vec3& point,
                                  const Vec3& normal, float damage, bool killed) {
    ProjectileHit h{};
    h.seq = ++hit_seq_;
    h.shooter = p.desc.shooter;
    h.weapon = p.desc.weapon;
    h.target = target;
    h.point = point;
    h.normal = normal;
    h.damage = damage;
    h.killed = killed;
    hits_[static_cast<std::size_t>((h.seq - 1) % hits_.size())] = h;
}

void ProjectileSystem::tick(float dt) {
    casts_ = 0;
    if (svc_.world == nullptr || svc_.physics == nullptr) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    for (auto& p : pool_) {
        if (p.state == State::Free) continue;
        p.age += dt;
        p.state_age += dt;
        switch (p.state) {
            case State::Flying:
                step(p, dt);
                break;
            case State::Stuck: {
                if (p.state_age >= p.desc.stick_s) {
                    expire(p);
                    break;
                }
                if (p.stuck_to != kNone) {
                    if (!svc_.world->valid(p.stuck_to) || !svc_.world->has<CTransform>(p.stuck_to)) {
                        expire(p);
                        break;
                    }
                    const auto& t = svc_.world->get<CTransform>(p.stuck_to).t;
                    const Mat4 m = glm::translate(Mat4(1.0f), t.position) *
                                   glm::mat4_cast(t.rotation) * p.stuck_local;
                    p.pos = Vec3(m[3]);
                    p.rot = glm::normalize(glm::quat_cast(Mat3(m)));
                }
                place_visual(p, p.rot);
                break;
            }
            case State::Resting:
                if (p.age >= p.desc.lifetime_s) expire(p);
                else place_visual(p, p.rot);
                break;
            case State::Free:
                break;
        }
    }
}

void ProjectileSystem::step(Live& p, float dt) {
    if (p.age >= p.desc.lifetime_s || p.travelled >= p.desc.max_distance_m) {
        expire(p);
        return;
    }
    const float speed = glm::length(p.vel);
    // A second sub-step only where gravity bends a long path noticeably.
    const int sub = (p.desc.gravity_scale > 0.0f && speed * dt > 3.0f) ? 2 : 1;
    const float h = dt / static_cast<float>(sub);
    std::uint32_t ignore[12];
    const int n_ignore = build_ignore(p, ignore, 12);
    for (int s = 0; s < sub; ++s) {
        Vec3 v_new = p.vel + Vec3(0.0f, -kGravity * p.desc.gravity_scale, 0.0f) * h;
        v_new -= v_new * std::min(1.0f, p.desc.drag * h);
        const Vec3 seg = 0.5f * (p.vel + v_new) * h;
        const float len = glm::length(seg);
        if (len > 1e-6f) {
            PhysicsRayHit hitres{};
            ++casts_;
            if (svc_.physics->sphere_cast_ignoring(ignore, n_ignore, p.pos, seg / len,
                                                   p.desc.radius_m, len, hitres) &&
                hitres.hit) {
                p.vel = v_new;
                hit(p, hitres, seg / len);
                return;
            }
        }
        p.pos += seg;
        p.travelled += len;
        p.vel = v_new;
    }
    const Vec3 dir = glm::length(p.vel) > 1e-5f ? glm::normalize(p.vel) : Vec3(0, 0, -1);
    // Visuals.
    switch (p.desc.visual) {
        case ProjectileVisual::Mesh:
            if (p.visual >= 0) {
                const auto& vis = visuals_[static_cast<std::size_t>(p.visual)];
                p.rot = rotation_between(vis.nose_dir, dir);
                place_visual(p, p.rot);
            }
            break;
        case ProjectileVisual::Tracer:
            if (svc_.vfx != nullptr) {
                const float l = std::min(p.desc.visual_length_m, p.travelled);
                svc_.vfx->streak(p.pos - dir * l, p.pos, p.desc.visual_width_m, p.desc.color);
            }
            break;
        case ProjectileVisual::Bolt:
            if (svc_.vfx != nullptr) {
                const float l = std::min(p.desc.visual_length_m, p.travelled);
                svc_.vfx->streak(p.pos - dir * l, p.pos, p.desc.visual_width_m * 2.0f,
                                 p.desc.color);
                svc_.vfx->glow(p.pos, p.desc.visual_width_m * 6.0f, p.desc.color);
            }
            break;
        case ProjectileVisual::None:
            break;
    }
}

void ProjectileSystem::hit(Live& p, const PhysicsRayHit& h, const Vec3& dir) {
    const std::uint32_t target = h.entity != kNone ? health_owner(h.entity) : kNone;
    float dealt = 0.0f;
    bool killed = false;
    const bool explodes = p.desc.on_hit == ProjectileOnHit::Explode;
    if (!explodes && p.desc.damage > 0.0f && may_damage(p.desc.shooter, target)) {
        const float before = svc_.combat->health(target);
        if (svc_.combat->apply_weapon_damage(target, p.desc.damage, p.desc.shooter)) {
            dealt = std::max(0.0f, before - svc_.combat->health(target));
            killed = !svc_.combat->is_alive(target);
        }
    }
    // Knock dynamic bodies about (an impulse on a kinematic body is a no-op).
    if (h.entity != kNone && svc_.physics->has_body(h.entity)) {
        svc_.physics->apply_impulse(h.entity, dir * std::clamp(p.desc.damage * 0.02f, 0.05f, 30.0f));
    }
    const bool organic = target != kNone && svc_.world->has<CHealth>(target);
    if (svc_.vfx != nullptr && !explodes) {
        ImpactStyle style = p.desc.impact;
        if (style == ImpactStyle::Sparks && organic) style = ImpactStyle::Blood;
        svc_.vfx->impact(h.point, h.normal, style, organic ? Vec4(0.6f, 0.03f, 0.03f, 1.0f)
                                                           : p.desc.color);
        if (!organic && (style == ImpactStyle::Sparks || style == ImpactStyle::Dust)) {
            svc_.vfx->decal(h.point, h.normal, 0.025f, Vec4(0.05f, 0.05f, 0.05f, 0.85f));
        }
    }
    if (!explodes) record_hit(p, target, h.point, h.normal, dealt, killed);

    switch (p.desc.on_hit) {
        case ProjectileOnHit::Explode:
            explode(p, h.point + h.normal * 0.05f, target);
            return;
        case ProjectileOnHit::Stick: {
            p.state = State::Stuck;
            p.state_age = 0.0f;
            // Bury the tip slightly so an arrow looks embedded.
            p.pos = h.point + dir * 0.03f;
            if (p.visual >= 0) {
                p.rot = rotation_between(visuals_[static_cast<std::size_t>(p.visual)].nose_dir, dir);
            }
            const std::uint32_t ride = h.entity;
            if (ride != kNone && svc_.world->valid(ride) && svc_.world->has<CTransform>(ride)) {
                const auto& t = svc_.world->get<CTransform>(ride).t;
                const Mat4 parent = glm::translate(Mat4(1.0f), t.position) * glm::mat4_cast(t.rotation);
                const Mat4 mine = glm::translate(Mat4(1.0f), p.pos) * glm::mat4_cast(p.rot);
                p.stuck_to = ride;
                p.stuck_local = glm::inverse(parent) * mine;
            }
            place_visual(p, p.rot);
            return;
        }
        case ProjectileOnHit::Bounce: {
            const Vec3 n = glm::length(h.normal) > 1e-5f ? glm::normalize(h.normal) : Vec3(0, 1, 0);
            p.vel = glm::reflect(p.vel, n) * kBounceRestitution;
            p.pos = h.point + n * std::max(0.01f, p.desc.radius_m + 0.005f);
            if (++p.bounces > 6 || glm::length(p.vel) < 0.6f) {
                p.state = State::Resting;
                p.vel = Vec3(0.0f);
            }
            return;
        }
        case ProjectileOnHit::Despawn:
            expire(p);
            return;
    }
}

void ProjectileSystem::explode(Live& p, const Vec3& at, std::uint32_t direct_target) {
    World& w = *svc_.world;
    const float r = std::max(0.1f, p.desc.splash_radius_m);
    if (svc_.vfx != nullptr) svc_.vfx->explosion(at, std::min(r, 6.0f) * 0.5f, p.desc.color);
    // The direct hit takes full damage; everything in the blast takes falloff
    // damage if the blast can see it.
    if (direct_target != kNone && may_damage(p.desc.shooter, direct_target) && p.desc.damage > 0.0f) {
        svc_.combat->apply_weapon_damage(direct_target, p.desc.damage, p.desc.shooter);
        record_hit(p, direct_target, at, Vec3(0, 1, 0), p.desc.damage,
                   !svc_.combat->is_alive(direct_target));
    }
    if (p.desc.splash_damage > 0.0f && svc_.combat != nullptr) {
        std::uint32_t ignore[12];
        const int n_ignore = build_ignore(p, ignore, 12);
        auto view = w.registry().view<const CHealth, const CTransform>();
        for (auto e : view) {
            const auto id = static_cast<std::uint32_t>(e);
            if (id == direct_target) continue;
            const Vec3 tp = view.get<const CTransform>(e).t.position + Vec3(0.0f, 0.4f, 0.0f);
            const float d = glm::length(tp - at);
            if (d > r || !may_damage(p.desc.shooter, id)) continue;
            // Line of sight from the blast to the target.
            PhysicsRayHit los{};
            ++casts_;
            if (d > 0.3f && svc_.physics->raycast_ignoring(ignore, n_ignore, at, (tp - at) / d,
                                                           d - 0.2f, los) &&
                los.hit && health_owner(los.entity) != id) {
                continue;
            }
            const float dmg = p.desc.splash_damage * (1.0f - d / r);
            if (dmg <= 0.0f) continue;
            svc_.combat->apply_weapon_damage(id, dmg, p.desc.shooter);
            record_hit(p, id, tp, Vec3(0, 1, 0), dmg, !svc_.combat->is_alive(id));
        }
    }
    if (direct_target == kNone && p.desc.splash_damage <= 0.0f) {
        record_hit(p, kNone, at, Vec3(0, 1, 0), 0.0f, false);
    }
    p.state = State::Free;
    release_visual(p);
}

void ProjectileSystem::expire(Live& p) {
    // Grenades and rockets that run out of time still go off.
    if (p.state != State::Free && p.state != State::Stuck &&
        (p.desc.on_hit == ProjectileOnHit::Explode ||
         (p.desc.on_hit == ProjectileOnHit::Bounce && p.desc.splash_radius_m > 0.0f))) {
        explode(p, p.pos, kNone);
        return;
    }
    p.state = State::Free;
    release_visual(p);
}

int ProjectileSystem::acquire_visual(std::uint32_t template_entity) {
    World& w = *svc_.world;
    if (!w.valid(template_entity) || !w.has<CMesh>(template_entity)) return -1;
    for (std::size_t i = 0; i < visuals_.size(); ++i) {
        auto& v = visuals_[i];
        if (!v.in_use && v.template_entity == template_entity && w.valid(v.entity)) {
            v.in_use = true;
            set_hidden(w, v.entity, false);
            return static_cast<int>(i);
        }
    }
    if (static_cast<int>(visuals_.size()) >= max_mesh_visuals_) return -1;
    const auto src = static_cast<entt::entity>(template_entity);
    const auto dst = w.create_entity();
    const auto& tmpl_mesh = w.registry().get<CMesh>(src);
    w.registry().emplace<CMesh>(dst, tmpl_mesh);
    Transform t{};
    if (w.registry().all_of<CTransform>(src)) t = w.registry().get<CTransform>(src).t;
    w.registry().emplace<CTransform>(dst, CTransform{t});
    w.registry().emplace<CTag>(dst, CTag{{"projectile_visual"}});
    Visual v{};
    v.entity = static_cast<std::uint32_t>(dst);
    v.template_entity = template_entity;
    v.in_use = true;
    const auto it = tmpl_mesh.sockets.find("nose");
    if (it != tmpl_mesh.sockets.end()) {
        v.nose_local = it->second.position * t.scale;
        v.nose_dir = glm::normalize(it->second.rotation * Vec3(0.0f, 0.0f, 1.0f));
    }
    visuals_.push_back(v);
    return static_cast<int>(visuals_.size()) - 1;
}

void ProjectileSystem::release_visual(Live& p) {
    if (p.visual < 0 || p.visual >= static_cast<int>(visuals_.size())) {
        p.visual = -1;
        return;
    }
    auto& v = visuals_[static_cast<std::size_t>(p.visual)];
    v.in_use = false;
    if (svc_.world != nullptr && svc_.world->valid(v.entity)) set_hidden(*svc_.world, v.entity, true);
    p.visual = -1;
}

void ProjectileSystem::place_visual(const Live& p, const Quat& rot) {
    if (p.visual < 0) return;
    const auto& v = visuals_[static_cast<std::size_t>(p.visual)];
    if (!svc_.world->valid(v.entity)) return;
    auto& t = svc_.world->get<CTransform>(v.entity).t;
    t.rotation = rot;
    // The nose is the point the hit test follows.
    t.position = p.pos - rot * v.nose_local;
}

}  // namespace ptv
