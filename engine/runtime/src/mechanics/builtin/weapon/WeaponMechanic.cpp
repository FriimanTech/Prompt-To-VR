// builtin.weapon -- a ranged weapon that actually shoots.
//
// Bound to each firing item (pistol, rifle, blaster, launcher, beam gun) with
// its tuning from interaction_design.json's WeaponSpec. It fires when the
// player holds it and pulls that hand's trigger, or when an NPC that wields it
// pulls the trigger through ``ctx.weapons->trigger``. Four delivery systems:
//
//   hitscan     an instant ray from the muzzle decides the hit;
//   hybrid      the same instant hit, plus a visible tracer / bolt that flies
//               muzzle -> hit at a visual speed (real bullets);
//   projectile  ``ctx.projectiles`` simulates the shot with travel time and
//               gravity (rockets, grenades, slow energy bolts);
//   beam        a continuous ray every frame while the trigger is held,
//               damage per second in 0.1 s ticks.
//
// The shot leaves the ``muzzle`` socket along its +Z. Every ray IGNORES the
// weapon itself, the hands, whatever they hold and the player: a muzzle sits
// on or inside the weapon's own collider, and an unfiltered ray from it hits
// the weapon at distance zero -- the reason the humans-vs-aliens pistol
// recoiled but never hurt anything. Damage goes through ``ctx.combat`` (so
// enemies die the same way whoever shot them) under the shared combat rules
// (gameplay/CombatRules.h). Recoil, haptics, muzzle flash, tracers, impacts
// and decals are part of firing; a separate builtin.recoil is not needed.
//
// Ammo: magazine + reserve (or infinite), reload on the holding hand's
// secondary face button (B on the right controller, Y on the left) or
// automatically when empty, or overheat for energy weapons. Firing waits for
// ``ctx.game->gameplay_active()`` and, for a two-hands-required weapon, for
// the second hand to be on it.

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "../../../gameplay/CombatRules.h"
#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kTriggerPress = 0.7f;
constexpr float kTriggerRelease = 0.35f;
constexpr float kBeamTick = 0.1f;

enum class Mode { Semi, Burst, Auto, Charge, HoldBeam };
enum class Reload { None, Button, Auto, Overheat, MagazineInsert };

Vec4 color_param(const nlohmann::json& p, const char* key, const Vec4& def) {
    if (!p.contains(key) || !p[key].is_array() || p[key].size() < 3) return def;
    const auto& a = p[key];
    return Vec4(a[0].get<float>(), a[1].get<float>(), a[2].get<float>(),
                a.size() > 3 ? a[3].get<float>() : 1.0f);
}

ImpactStyle parse_impact(const std::string& s) {
    if (s == "blood") return ImpactStyle::Blood;
    if (s == "energy") return ImpactStyle::Energy;
    if (s == "dust") return ImpactStyle::Dust;
    if (s == "explosion") return ImpactStyle::Explosion;
    if (s == "none") return ImpactStyle::None;
    return ImpactStyle::Sparks;
}

class WeaponMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.weapon"; }
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext& ctx) override {
        const std::string delivery = p.value("delivery", std::string("hybrid"));
        delivery_ = delivery == "hitscan"      ? WeaponDelivery::Hitscan
                    : delivery == "projectile" ? WeaponDelivery::Projectile
                    : delivery == "beam"       ? WeaponDelivery::Beam
                                               : WeaponDelivery::Hybrid;
        const std::string mode = p.value("fire_mode", std::string("semi"));
        mode_ = mode == "auto"      ? Mode::Auto
                : mode == "burst"   ? Mode::Burst
                : mode == "charge"  ? Mode::Charge
                : mode == "hold_beam" ? Mode::HoldBeam
                                    : Mode::Semi;
        if (delivery_ == WeaponDelivery::Beam) mode_ = Mode::HoldBeam;
        interval_s_ = 60.0f / std::clamp(p.value("rpm", 300.0f), 1.0f, 6000.0f);
        burst_count_ = std::clamp(p.value("burst_count", 3), 1, 20);
        pellets_ = std::clamp(p.value("pellets", 1), 1, 64);
        spread_rad_ = glm::radians(std::clamp(p.value("spread_deg", 0.5f), 0.0f, 45.0f));
        range_m_ = std::clamp(p.value("range_m", 100.0f), 1.0f, 5000.0f);
        damage_ = std::max(0.0f, p.value("damage", 25.0f));
        falloff_ = std::clamp(p.value("falloff_at_range", 1.0f), 0.0f, 1.0f);
        muzzle_socket_ = p.value("muzzle_socket", std::string("muzzle"));

        const auto ammo = p.value("ammo", nlohmann::json::object());
        infinite_ = ammo.value("infinite", false);
        magazine_size_ = std::max(0, ammo.value("magazine", 0));
        if (magazine_size_ == 0 && !infinite_) infinite_ = true;
        reserve_ = std::max(0, ammo.value("reserve", magazine_size_ * 4));
        ammo_ = magazine_size_;
        const std::string reload = ammo.value("reload", std::string("button"));
        reload_ = reload == "auto"              ? Reload::Auto
                  : reload == "overheat"        ? Reload::Overheat
                  : reload == "none"            ? Reload::None
                  : reload == "magazine_insert" ? Reload::MagazineInsert
                                                : Reload::Button;
        reload_time_s_ = std::clamp(ammo.value("reload_time_s", 1.5f), 0.1f, 30.0f);
        heat_per_shot_ = std::clamp(ammo.value("heat_per_shot", 0.08f), 0.0f, 1.0f);
        heat_vent_per_s_ = std::clamp(ammo.value("heat_vent_per_s", 0.35f), 0.0f, 10.0f);
        if (reload_ == Reload::Overheat) infinite_ = true;

        const auto pj = p.value("projectile", nlohmann::json::object());
        proj_.gravity_scale = pj.value("gravity_scale", 1.0f);
        proj_.drag = pj.value("drag", 0.0f);
        proj_.radius_m = pj.value("radius_m", 0.0f);
        proj_.lifetime_s = pj.value("lifetime_s", 4.0f);
        proj_.splash_radius_m = pj.value("splash_radius_m", 0.0f);
        proj_.splash_damage = pj.value("splash_damage", 0.0f);
        proj_.stick_s = pj.value("stick_s", 6.0f);
        const std::string on_hit = pj.value("on_hit", std::string("despawn"));
        proj_.on_hit = on_hit == "stick"     ? ProjectileOnHit::Stick
                       : on_hit == "bounce"  ? ProjectileOnHit::Bounce
                       : on_hit == "explode" ? ProjectileOnHit::Explode
                                             : ProjectileOnHit::Despawn;
        speed_mps_ = std::clamp(pj.value("speed_mps", 360.0f), 1.0f, 5000.0f);
        const std::string tmpl = pj.value("visual_template", std::string());
        if (!tmpl.empty()) {
            const auto e = world.find(tmpl);
            if (e != entt::null) proj_.visual_template = static_cast<std::uint32_t>(e);
        }

        const std::string visual = p.value("visual", std::string("tracer"));
        visual_ = visual == "energy_bolt" ? ProjectileVisual::Bolt
                  : visual == "mesh"      ? ProjectileVisual::Mesh
                  : visual == "none"      ? ProjectileVisual::None
                                          : ProjectileVisual::Tracer;
        if (visual_ == ProjectileVisual::Mesh && proj_.visual_template == kNone) {
            visual_ = ProjectileVisual::Bolt;
        }
        color_ = color_param(p, "color_rgb", Vec4(1.0f, 0.85f, 0.5f, 1.0f));
        tracer_every_n_ = std::clamp(p.value("tracer_every_n", 1), 1, 20);
        tracer_speed_ = std::clamp(p.value("tracer_speed_mps", 220.0f), 10.0f, 2000.0f);
        tracer_width_ = std::clamp(p.value("tracer_width_m", 0.012f), 0.001f, 0.2f);
        impact_ = parse_impact(p.value("impact", std::string("sparks")));
        muzzle_flash_ = p.value("muzzle_flash", true);
        flash_scale_ = std::clamp(p.value("flash_scale", 1.0f), 0.2f, 6.0f);

        const auto beam = p.value("beam", nlohmann::json::object());
        beam_dps_ = std::max(0.0f, beam.value("damage_per_s", damage_ * 2.0f));
        beam_width_ = std::clamp(beam.value("width_m", 0.02f), 0.002f, 1.0f);
        beam_range_ = std::clamp(beam.value("max_length_m", range_m_), 1.0f, 2000.0f);
        beam_color_ = color_param(beam, "color_rgb", color_);
        beam_heat_per_s_ = std::clamp(beam.value("heat_per_s", 0.18f), 0.0f, 5.0f);

        const auto recoil = p.value("recoil", nlohmann::json::object());
        kick_back_m_ = std::clamp(recoil.value("kick_back_m", 0.02f), 0.0f, 0.3f);
        kick_up_deg_ = std::clamp(recoil.value("kick_up_deg", 4.0f), 0.0f, 45.0f);
        recover_s_ = std::clamp(recoil.value("recover_s", 0.08f), 0.01f, 2.0f);
        haptic_ = std::clamp(p.value("haptic_amplitude", 0.6f), 0.0f, 1.0f);
        two_hand_required_ = p.value("two_hand_required", false);
        if (const auto* hp = world.registry().try_get<CHoldProfile>(
                static_cast<entt::entity>(entity))) {
            if (hp->two_hand == TwoHandMode::Required) two_hand_required_ = true;
        }
        // Recoil follows THIS weapon's barrel: the item's mesh-space forward
        // (hold profile, else the muzzle socket's +Z), not an assumed -Z.
        Vec3 fwd(0.0f, 0.0f, -1.0f), up(0.0f, 1.0f, 0.0f);
        const auto ent = static_cast<entt::entity>(entity);
        if (const auto* hp = world.registry().try_get<CHoldProfile>(ent);
            hp != nullptr && hp->has_forward) {
            fwd = hp->forward;
            up = hp->up;
        } else if (const auto* mesh = world.registry().try_get<CMesh>(ent)) {
            const auto it = mesh->sockets.find(muzzle_socket_);
            if (it != mesh->sockets.end()) {
                fwd = it->second.rotation * Vec3(0.0f, 0.0f, 1.0f);
                up = it->second.rotation * Vec3(0.0f, 1.0f, 0.0f);
            }
        }
        fwd = glm::normalize(fwd);
        Vec3 axis = glm::cross(fwd, up);
        if (glm::length(axis) < 1e-4f) axis = Vec3(1.0f, 0.0f, 0.0f);
        kick_offset_ = -fwd * kick_back_m_;
        const Quat rise = glm::angleAxis(glm::radians(kick_up_deg_), glm::normalize(axis));
        kick_euler_deg_ = glm::degrees(glm::eulerAngles(rise));
        if (ctx.sockets != nullptr && !ctx.sockets->has_socket(entity, muzzle_socket_.c_str())) {
            PTV_WARN("builtin.weapon: entity {} has no '{}' socket; shots leave from its "
                     "front face", entity, muzzle_socket_);
        }
        publish(entity, ctx, -1, kNone);
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
                   MechanicContext& ctx) override {
        dt = std::clamp(dt, 0.0f, 0.1f);
        cooldown_ = std::max(0.0f, cooldown_ - dt);
        relay_projectile_hits(ctx, self);

        // Who is firing: the player hand holding it, or the NPC it rides.
        int hand = -1;
        for (int h = 0; h < 2; ++h) {
            if (input.hands[h].held_entity == self) hand = h;
        }
        std::uint32_t wielder = kNone;
        bool trigger_down = false;
        bool trigger_edge = false;
        if (hand >= 0) {
            wielder = ctx.hands ? ctx.hands->player_entity() : kNone;
            const auto& hs = input.hands[hand];
            trigger_down = trigger_latched_ ? hs.trigger >= kTriggerRelease
                                            : hs.trigger >= kTriggerPress;
        } else if (world.has<CAttachment>(self)) {
            wielder = world.get<CAttachment>(self).parent;
            trigger_down = ctx.weapons != nullptr && ctx.weapons->npc_trigger(self);
        } else if (world.has<CFaction>(self)) {
            // A character modelled with its gun: it is its own wielder.
            wielder = self;
            trigger_down = ctx.weapons != nullptr && ctx.weapons->npc_trigger(self);
        }
        const bool fire_request = ctx.weapons != nullptr && ctx.weapons->take_fire_request(self);
        trigger_edge = trigger_down && !trigger_latched_;
        trigger_latched_ = trigger_down;

        const bool live = ctx.game == nullptr || ctx.game->gameplay_active();
        const bool hands_ok =
            !two_hand_required_ || hand < 0 ||
            (ctx.hands != nullptr && ctx.hands->support_entity(1 - hand) == self);

        // Reload / heat.
        tick_reload(dt, input, hand, ctx);
        if (wielder == kNone && !fire_request) {
            beam_active_ = false;
            burst_left_ = 0;
            publish(self, ctx, hand, wielder);
            return;
        }

        if (!live || !hands_ok) {
            beam_active_ = false;
            burst_left_ = 0;
            if (trigger_edge && !hands_ok && ctx.haptics != nullptr && hand >= 0) {
                ctx.haptics->pulse(hand, 0.02f, 0.15f);  // "needs two hands"
            }
            publish(self, ctx, hand, wielder);
            return;
        }

        switch (mode_) {
            case Mode::Semi:
                if ((trigger_edge || fire_request) && cooldown_ <= 0.0f) try_fire(world, self, hand, wielder, ctx, 1.0f);
                break;
            case Mode::Burst:
                if ((trigger_edge || fire_request) && burst_left_ == 0) burst_left_ = burst_count_;
                if (burst_left_ > 0 && cooldown_ <= 0.0f) {
                    if (!try_fire(world, self, hand, wielder, ctx, 1.0f)) burst_left_ = 0;
                    else --burst_left_;
                }
                break;
            case Mode::Auto:
                if ((trigger_down || fire_request) && cooldown_ <= 0.0f) {
                    try_fire(world, self, hand, wielder, ctx, 1.0f);
                }
                break;
            case Mode::Charge:
                if (trigger_down) {
                    charge_s_ += dt;
                    if (ctx.vfx != nullptr && ctx.sockets != nullptr) {
                        const Mat4 m = ctx.sockets->socket_world(self, muzzle_socket_.c_str());
                        ctx.vfx->glow(Vec3(m[3]), 0.02f + 0.06f * std::min(1.0f, charge_s_),
                                      color_);
                    }
                } else if (charge_s_ > 0.0f) {
                    const float charge = std::clamp(charge_s_ / 1.0f, 0.25f, 1.0f);
                    charge_s_ = 0.0f;
                    if (cooldown_ <= 0.0f) try_fire(world, self, hand, wielder, ctx, charge);
                }
                break;
            case Mode::HoldBeam:
                tick_beam(world, self, hand, wielder, ctx, dt, trigger_down || fire_request);
                break;
        }
        publish(self, ctx, hand, wielder);
    }

private:
    // ------------------------------------------------------------------
    void tick_reload(float dt, const InputSnapshot& input, int hand, MechanicContext& ctx) {
        if (reload_ == Reload::Overheat) {
            if (!beam_active_) heat_ = std::max(0.0f, heat_ - heat_vent_per_s_ * dt);
            if (overheated_ && heat_ <= 0.25f) overheated_ = false;
            return;
        }
        if (reloading_) {
            reload_left_ -= dt;
            if (reload_left_ <= 0.0f) {
                reloading_ = false;
                const int want = magazine_size_ - ammo_;
                const int got = infinite_ ? want : std::min(want, reserve_);
                ammo_ += got;
                if (!infinite_) reserve_ -= got;
                if (ctx.haptics != nullptr && hand >= 0) ctx.haptics->pulse(hand, 0.05f, 0.4f);
            }
            return;
        }
        if (infinite_ && reload_ == Reload::None) return;
        // The holding hand's secondary face button: B on the right, Y on the left.
        const bool pressed = hand == 1 ? input.button_b_pressed()
                             : hand == 0 ? input.button_y_pressed() : false;
        if (reload_ == Reload::Button && pressed) start_reload();
    }

    void start_reload() {
        if (reloading_ || magazine_size_ <= 0 || ammo_ >= magazine_size_) return;
        if (!infinite_ && reserve_ <= 0) return;
        reloading_ = true;
        reload_left_ = reload_time_s_;
    }

    bool has_round() const {
        if (reload_ == Reload::Overheat) return !overheated_;
        if (reloading_) return false;
        return infinite_ && magazine_size_ <= 0 ? true : ammo_ > 0;
    }

    void consume_round() {
        if (reload_ == Reload::Overheat) {
            heat_ = std::min(1.0f, heat_ + heat_per_shot_);
            if (heat_ >= 1.0f) overheated_ = true;
            return;
        }
        if (magazine_size_ > 0) ammo_ = std::max(0, ammo_ - 1);
    }

    // ------------------------------------------------------------------
    void muzzle(std::uint32_t self, MechanicContext& ctx, Vec3* origin, Vec3* dir) const {
        Mat4 m(1.0f);
        if (ctx.sockets != nullptr) m = ctx.sockets->socket_world(self, muzzle_socket_.c_str());
        *origin = Vec3(m[3]);
        Vec3 d = Vec3(m[2]);
        const float l = glm::length(d);
        *dir = l > 1e-6f ? d / l : Vec3(0.0f, 0.0f, -1.0f);
    }

    int ignore_list(World& world, std::uint32_t self, std::uint32_t wielder,
                    MechanicContext& ctx, std::uint32_t* out) const {
        int n = 0;
        out[n++] = self;
        if (wielder != kNone) out[n++] = wielder;
        // The player's body, hands and held items only for the player's own
        // shots: ignoring them for an NPC's shot made every ranged enemy's
        // ray pass straight through the player.
        if (ctx.hands != nullptr && wielder == ctx.hands->player_entity()) {
            out[n++] = ctx.hands->player_entity();
            for (int h = 0; h < 2; ++h) {
                const std::uint32_t he = ctx.hands->hand_entity(h);
                const std::uint32_t held = ctx.hands->held_entity(h);
                if (he != kNone) out[n++] = he;
                if (held != kNone && held != self) out[n++] = held;
            }
        }
        if (world.has<CAttachment>(self)) out[n++] = world.get<CAttachment>(self).parent;
        return n;
    }

    Vec3 spread(const Vec3& dir, float amount) {
        if (amount <= 0.0f) return dir;
        const Vec3 a = std::fabs(dir.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 u = glm::normalize(glm::cross(dir, a));
        const Vec3 v = glm::cross(dir, u);
        const float r = amount * std::sqrt(rnd());
        const float t = 6.2831853f * rnd();
        return glm::normalize(dir + u * (std::cos(t) * r) + v * (std::sin(t) * r));
    }

    float rnd() {
        seed_ = seed_ * 1664525u + 1013904223u;
        return static_cast<float>(seed_ >> 8) / static_cast<float>(1u << 24);
    }

    bool try_fire(World& world, std::uint32_t self, int hand, std::uint32_t wielder,
                  MechanicContext& ctx, float charge) {
        if (!has_round()) {
            // Dry fire: a click, and an automatic reload if the weapon has one.
            if (ctx.haptics != nullptr && hand >= 0) ctx.haptics->pulse(hand, 0.015f, 0.2f);
            if (reload_ == Reload::Auto || (reload_ == Reload::Button && hand < 0)) start_reload();
            cooldown_ = interval_s_;
            return false;
        }
        consume_round();
        cooldown_ = interval_s_;
        ++fire_seq_;
        ++shots_;
        Vec3 origin, dir;
        muzzle(self, ctx, &origin, &dir);
        // An NPC aims with the AI's target, not with an animated arm.
        Vec3 aim;
        if (hand < 0 && ctx.weapons != nullptr && ctx.weapons->aim_point(self, &aim)) {
            const Vec3 d = aim - origin;
            if (glm::length(d) > 0.05f) dir = glm::normalize(d);
        }
        last_origin_ = origin;
        last_dir_ = dir;
        std::uint32_t ignore[12];
        const int n_ignore = ignore_list(world, self, wielder, ctx, ignore);

        for (int i = 0; i < pellets_; ++i) {
            const Vec3 d = spread(dir, spread_rad_);
            if (delivery_ == WeaponDelivery::Projectile) {
                ProjectileDesc pd = proj_;
                pd.origin = origin;
                pd.velocity = d * speed_mps_;
                pd.shooter = wielder;
                pd.weapon = self;
                pd.damage = damage_ * charge;
                pd.visual = visual_;
                pd.color = color_;
                pd.visual_width_m = tracer_width_;
                pd.visual_length_m = visual_ == ProjectileVisual::Bolt ? 0.25f : 0.4f;
                pd.impact = impact_;
                pd.max_distance_m = range_m_ * 1.5f;
                if (ctx.projectiles != nullptr) ctx.projectiles->launch(pd);
                continue;
            }
            hitscan(world, self, wielder, ctx, origin, d, ignore, n_ignore, charge);
        }
        // Feedback.
        if (muzzle_flash_ && ctx.vfx != nullptr) {
            ctx.vfx->muzzle_flash(origin, dir, color_, flash_scale_);
        }
        if (ctx.fx != nullptr && (kick_back_m_ > 0.0f || kick_up_deg_ > 0.0f)) {
            TransformImpulse kick{};
            kick.local_offset = kick_offset_;
            kick.local_euler_deg = kick_euler_deg_;
            kick.recover_tau_s = recover_s_;
            ctx.fx->impulse(self, kick);
        }
        if (ctx.haptics != nullptr && hand >= 0) {
            ctx.haptics->pulse(hand, 0.04f, haptic_);
            if (ctx.hands != nullptr && ctx.hands->support_entity(1 - hand) == self) {
                ctx.haptics->pulse(1 - hand, 0.03f, haptic_ * 0.6f);
            }
        }
        if (reload_ == Reload::Auto && magazine_size_ > 0 && ammo_ == 0) start_reload();
        return true;
    }

    /** A ray that passes through the wielder's friends: a companion walking
     *  beside the player (or a squadmate beside an NPC) is not hurt by their
     *  shots, and should not soak them up either. */
    bool cast_past_friends(World& world, MechanicContext& ctx, std::uint32_t wielder,
                           const Vec3& origin, const Vec3& d, float range,
                           const std::uint32_t* ignore, int n_ignore, PhysicsRayHit& h) const {
        if (ctx.physics == nullptr) return false;
        std::uint32_t ig[16];
        int n = std::min(n_ignore, 12);
        for (int i = 0; i < n; ++i) ig[i] = ignore[i];
        for (int pass = 0; pass < 3; ++pass) {
            h = PhysicsRayHit{};
            if (!ctx.physics->raycast_ignoring(ig, n, origin, d, range, h) || !h.hit) return false;
            if (h.entity == kNone) return true;
            const std::uint32_t owner = combat_rules::health_owner(world, h.entity);
            const bool friend_npc = owner != wielder && world.has<CFaction>(owner) &&
                                    world.has<CHealth>(owner) &&
                                    !combat_rules::may_damage(ctx.combat, wielder, owner);
            if (!friend_npc || n >= 15) return true;
            ig[n++] = h.entity;
            if (owner != h.entity) ig[n++] = owner;
        }
        return h.hit;
    }

    void hitscan(World& world, std::uint32_t self, std::uint32_t wielder, MechanicContext& ctx,
                 const Vec3& origin, const Vec3& d, const std::uint32_t* ignore, int n_ignore,
                 float charge) {
        PhysicsRayHit h{};
        const bool hit = cast_past_friends(world, ctx, wielder, origin, d, range_m_, ignore,
                                           n_ignore, h);
        const Vec3 end = hit ? h.point : origin + d * range_m_;
        const float dist = glm::length(end - origin);
        // The visible shot.
        if (ctx.vfx != nullptr && (shots_ % static_cast<std::uint64_t>(tracer_every_n_)) == 0) {
            if (delivery_ == WeaponDelivery::Hybrid || visual_ != ProjectileVisual::None) {
                const bool bolt = visual_ == ProjectileVisual::Bolt;
                ctx.vfx->tracer(origin, end, delivery_ == WeaponDelivery::Hybrid
                                                 ? tracer_speed_ : 900.0f,
                                color_, bolt ? tracer_width_ * 2.5f : tracer_width_,
                                bolt ? 0.35f : std::min(1.2f, 0.004f * tracer_speed_));
            }
        }
        if (!hit) return;
        std::uint32_t target = kNone;
        float dealt = 0.0f;
        bool killed = false;
        if (h.entity != kNone) {
            target = combat_rules::health_owner(world, h.entity);
            if (combat_rules::may_damage(ctx.combat, wielder, target)) {
                const float t = std::clamp(dist / range_m_, 0.0f, 1.0f);
                const float dmg = damage_ * charge * (1.0f + (falloff_ - 1.0f) * t);
                const float before = ctx.combat->health(target);
                if (ctx.combat->apply_weapon_damage(target, dmg, wielder)) {
                    dealt = std::max(0.0f, before - ctx.combat->health(target));
                    killed = !ctx.combat->is_alive(target);
                }
            }
            if (ctx.physics->has_body(h.entity)) {
                ctx.physics->apply_impulse(h.entity, d * std::clamp(damage_ * 0.02f, 0.05f, 20.0f));
            }
        }
        const bool organic = target != kNone && world.has<CHealth>(target);
        if (ctx.vfx != nullptr) {
            ImpactStyle style = impact_;
            if (style == ImpactStyle::Sparks && organic) style = ImpactStyle::Blood;
            ctx.vfx->impact(h.point, h.normal, style,
                            organic ? Vec4(0.6f, 0.03f, 0.03f, 1.0f) : color_);
            if (!organic && (style == ImpactStyle::Sparks || style == ImpactStyle::Dust)) {
                ctx.vfx->decal(h.point, h.normal, 0.025f, Vec4(0.05f, 0.05f, 0.05f, 0.85f));
            }
        }
        publish_hit(ctx, self, wielder, target, h.point, h.normal, dealt, killed);
    }

    void tick_beam(World& world, std::uint32_t self, int hand, std::uint32_t wielder,
                   MechanicContext& ctx, float dt, bool on) {
        if (!on || !has_round()) {
            if (on && hand >= 0 && ctx.haptics != nullptr && overheated_) {
                ctx.haptics->pulse(hand, 0.01f, 0.1f);
            }
            beam_active_ = false;
            beam_accum_ = 0.0f;
            return;
        }
        beam_active_ = true;
        ++fire_seq_;
        Vec3 origin, dir;
        muzzle(self, ctx, &origin, &dir);
        last_origin_ = origin;
        last_dir_ = dir;
        std::uint32_t ignore[12];
        const int n_ignore = ignore_list(world, self, wielder, ctx, ignore);
        PhysicsRayHit h{};
        const bool hit = cast_past_friends(world, ctx, wielder, origin, dir, beam_range_, ignore,
                                           n_ignore, h);
        const Vec3 end = hit ? h.point : origin + dir * beam_range_;
        if (ctx.vfx != nullptr) {
            ctx.vfx->beam(origin, end, beam_width_, beam_color_);
            ctx.vfx->glow(origin, beam_width_ * 5.0f, beam_color_);
            if (hit) ctx.vfx->glow(end, beam_width_ * 8.0f, beam_color_);
        }
        if (reload_ == Reload::Overheat) {
            heat_ = std::min(1.0f, heat_ + beam_heat_per_s_ * dt);
            if (heat_ >= 1.0f) overheated_ = true;
        }
        beam_accum_ += dt;
        if (beam_accum_ < kBeamTick) return;
        const float tick_s = beam_accum_;
        beam_accum_ = 0.0f;
        if (hand >= 0 && ctx.haptics != nullptr) ctx.haptics->pulse(hand, 0.05f, haptic_ * 0.5f);
        if (!hit) return;
        std::uint32_t target = kNone;
        float dealt = 0.0f;
        bool killed = false;
        if (h.entity != kNone) {
            target = combat_rules::health_owner(world, h.entity);
            if (combat_rules::may_damage(ctx.combat, wielder, target)) {
                const float before = ctx.combat->health(target);
                if (ctx.combat->apply_weapon_damage(target, beam_dps_ * tick_s, wielder)) {
                    dealt = std::max(0.0f, before - ctx.combat->health(target));
                    killed = !ctx.combat->is_alive(target);
                }
            }
        }
        if (ctx.vfx != nullptr) {
            ctx.vfx->impact(h.point, h.normal, ImpactStyle::Energy, beam_color_);
            if (target == kNone || !world.has<CHealth>(target)) {
                ctx.vfx->decal(h.point, h.normal, beam_width_ * 3.0f,
                               Vec4(0.08f, 0.06f, 0.05f, 0.7f));
            }
        }
        publish_hit(ctx, self, wielder, target, h.point, h.normal, dealt, killed);
    }

    // ------------------------------------------------------------------
    void publish_hit(MechanicContext& ctx, std::uint32_t self, std::uint32_t wielder,
                     std::uint32_t target, const Vec3& point, const Vec3& normal, float dmg,
                     bool killed) const {
        if (ctx.weapons == nullptr) return;
        WeaponHit wh{};
        wh.weapon = self;
        wh.shooter = wielder;
        wh.target = target;
        wh.point = point;
        wh.normal = normal;
        wh.damage = dmg;
        wh.killed = killed;
        wh.delivery = delivery_;
        ctx.weapons->publish_hit(wh);
    }

    /** Projectile hits land later; mirror the ones this weapon fired onto
     *  the weapon board so ``ctx.weapons->hits_since`` sees every hit. */
    void relay_projectile_hits(MechanicContext& ctx, std::uint32_t self) {
        if (delivery_ != WeaponDelivery::Projectile || ctx.projectiles == nullptr) return;
        ProjectileHit buf[16];
        const int n = ctx.projectiles->hits_since(proj_seq_seen_, buf, 16);
        for (int i = 0; i < n; ++i) {
            proj_seq_seen_ = std::max(proj_seq_seen_, buf[i].seq);
            if (buf[i].weapon != self) continue;
            if (ctx.weapons == nullptr) continue;
            WeaponHit wh{};
            wh.weapon = self;
            wh.shooter = buf[i].shooter;
            wh.target = buf[i].target;
            wh.point = buf[i].point;
            wh.normal = buf[i].normal;
            wh.damage = buf[i].damage;
            wh.killed = buf[i].killed;
            wh.delivery = WeaponDelivery::Projectile;
            ctx.weapons->publish_hit(wh);
        }
    }

    void publish(std::uint32_t self, MechanicContext& ctx, int hand, std::uint32_t wielder) {
        if (ctx.weapons == nullptr) return;
        WeaponState s{};
        s.configured = true;
        s.delivery = delivery_;
        s.ammo_in_magazine = ammo_;
        s.magazine_size = magazine_size_;
        s.reserve = reserve_;
        s.infinite = infinite_;
        s.reloading = reloading_;
        s.reload_progress = reloading_ ? 1.0f - reload_left_ / reload_time_s_ : 0.0f;
        s.heat = heat_;
        s.overheated = overheated_;
        s.fire_seq = fire_seq_;
        s.held_by_hand = hand;
        s.wielder = wielder;
        s.last_origin = last_origin_;
        s.last_direction = last_dir_;
        ctx.weapons->publish_state(self, s);
    }

    Vec3 last_origin_{0.0f};
    Vec3 last_dir_{0.0f, 0.0f, -1.0f};
    WeaponDelivery delivery_{WeaponDelivery::Hybrid};
    Mode mode_{Mode::Semi};
    float interval_s_{0.2f};
    int burst_count_{3};
    int pellets_{1};
    float spread_rad_{0.0f};
    float range_m_{100.0f};
    float damage_{25.0f};
    float falloff_{1.0f};
    std::string muzzle_socket_{"muzzle"};
    bool infinite_{false};
    int magazine_size_{0};
    int reserve_{0};
    int ammo_{0};
    Reload reload_{Reload::Button};
    float reload_time_s_{1.5f};
    float heat_per_shot_{0.08f};
    float heat_vent_per_s_{0.35f};
    ProjectileDesc proj_{};
    float speed_mps_{360.0f};
    ProjectileVisual visual_{ProjectileVisual::Tracer};
    Vec4 color_{1.0f, 0.85f, 0.5f, 1.0f};
    int tracer_every_n_{1};
    float tracer_speed_{220.0f};
    float tracer_width_{0.012f};
    ImpactStyle impact_{ImpactStyle::Sparks};
    bool muzzle_flash_{true};
    float flash_scale_{1.0f};
    float beam_dps_{50.0f};
    float beam_width_{0.02f};
    float beam_range_{60.0f};
    Vec4 beam_color_{1.0f, 0.15f, 0.1f, 1.0f};
    float beam_heat_per_s_{0.18f};
    float kick_back_m_{0.02f};
    float kick_up_deg_{4.0f};
    float recover_s_{0.08f};
    Vec3 kick_offset_{0.0f, 0.0f, 0.02f};
    Vec3 kick_euler_deg_{-4.0f, 0.0f, 0.0f};
    float haptic_{0.6f};
    bool two_hand_required_{false};

    float cooldown_{0.0f};
    bool trigger_latched_{false};
    int burst_left_{0};
    float charge_s_{0.0f};
    bool reloading_{false};
    float reload_left_{0.0f};
    float heat_{0.0f};
    bool overheated_{false};
    bool beam_active_{false};
    float beam_accum_{0.0f};
    std::uint64_t fire_seq_{0};
    std::uint64_t shots_{0};
    std::uint64_t proj_seq_seen_{0};
    std::uint32_t seed_{0x2545F491u};
};

}  // namespace

extern "C" int ptv_force_link_builtin_weapon = 1;

PTV_REGISTER_MECHANIC("builtin.weapon", WeaponMechanic)

}  // namespace ptv
