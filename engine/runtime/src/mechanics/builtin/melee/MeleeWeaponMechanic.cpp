// builtin.melee_weapon -- swords, axes, hammers, clubs and bats that hit.
//
// The striking part is a segment between the item's ``strike_base`` and
// ``strike_tip`` sockets (a blade), or a sphere at its ``strike_face`` (a
// hammer head). Each frame the segment's previous and current positions are
// joined by a few rays, so a fast swing sweeps the whole arc between frames
// and cannot pass through a target unseen. A hit counts only above
// ``min_speed_mps`` of tip speed, deals more the faster the swing, and each
// target is hit at most once per ``cooldown_s``. Damage goes through
// ``ctx.combat->apply_weapon_damage`` under the shared combat rules; impacts,
// haptics and ``ctx.weapons`` hits (delivery Melee) follow.

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "../../../gameplay/CombatRules.h"
#include "../BuiltinMechanicBase.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr int kSamples = 4;

ImpactStyle parse_impact(const std::string& s) {
    if (s == "blood") return ImpactStyle::Blood;
    if (s == "energy") return ImpactStyle::Energy;
    if (s == "dust") return ImpactStyle::Dust;
    if (s == "none") return ImpactStyle::None;
    return ImpactStyle::Sparks;
}

class MeleeWeaponMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.melee_weapon"; }
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext&) override {
        damage_ = std::max(0.0f, p.value("damage", 35.0f));
        min_speed_ = std::clamp(p.value("min_speed_mps", 1.5f), 0.0f, 50.0f);
        speed_scale_ = std::clamp(p.value("speed_scale", 0.15f), 0.0f, 10.0f);
        cooldown_s_ = std::clamp(p.value("cooldown_s", 0.4f), 0.0f, 10.0f);
        blunt_ = p.value("blunt", false);
        impact_ = parse_impact(p.value("impact", std::string("sparks")));
        base_socket_ = p.value("strike_base_socket", std::string("strike_base"));
        tip_socket_ = p.value("strike_tip_socket", std::string("strike_tip"));
        face_socket_ = p.value("strike_face_socket", std::string("strike_face"));
        face_radius_ = std::clamp(p.value("face_radius_m", 0.07f), 0.01f, 1.0f);
        (void)world;
        (void)entity;
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
                   MechanicContext& ctx) override {
        if (ctx.sockets == nullptr || ctx.physics == nullptr) return;
        dt = std::clamp(dt, 1e-4f, 0.1f);
        for (auto it = cooldowns_.begin(); it != cooldowns_.end();) {
            it->second -= dt;
            it = it->second <= 0.0f ? cooldowns_.erase(it) : std::next(it);
        }
        int hand = -1;
        for (int h = 0; h < 2; ++h) {
            if (input.hands[h].held_entity == self) hand = h;
        }
        std::uint32_t wielder = kNone;
        if (hand >= 0 && ctx.hands != nullptr) {
            wielder = ctx.hands->player_entity();
        } else if (world.has<CAttachment>(self)) {
            wielder = world.get<CAttachment>(self).parent;
        }
        Vec3 a, b;
        if (!segment(self, ctx, &a, &b)) return;
        publish(ctx, self, hand, wielder);
        if (wielder == kNone || (ctx.game != nullptr && !ctx.game->gameplay_active())) {
            have_prev_ = false;
            return;
        }
        if (!have_prev_) {
            prev_a_ = a;
            prev_b_ = b;
            have_prev_ = true;
            return;
        }
        const float speed = glm::length(b - prev_b_) / dt;
        if (speed >= min_speed_) {
            sweep(world, self, hand, wielder, ctx, a, b, speed);
        }
        prev_a_ = a;
        prev_b_ = b;
    }

private:
    bool segment(std::uint32_t self, MechanicContext& ctx, Vec3* a, Vec3* b) const {
        if (!blunt_ && ctx.sockets->has_socket(self, tip_socket_.c_str())) {
            *b = Vec3(ctx.sockets->socket_world(self, tip_socket_.c_str())[3]);
            *a = ctx.sockets->has_socket(self, base_socket_.c_str())
                     ? Vec3(ctx.sockets->socket_world(self, base_socket_.c_str())[3])
                     : *b;
            return true;
        }
        if (ctx.sockets->has_socket(self, face_socket_.c_str())) {
            const Mat4 m = ctx.sockets->socket_world(self, face_socket_.c_str());
            *b = Vec3(m[3]);
            *a = *b - glm::normalize(Vec3(m[2])) * face_radius_;
            return true;
        }
        return false;
    }

    void sweep(World& world, std::uint32_t self, int hand, std::uint32_t wielder,
               MechanicContext& ctx, const Vec3& a, const Vec3& b, float speed) {
        std::uint32_t ignore[10];
        int n = 0;
        ignore[n++] = self;
        if (wielder != kNone) ignore[n++] = wielder;
        if (ctx.hands != nullptr) {
            for (int h = 0; h < 2; ++h) {
                const std::uint32_t he = ctx.hands->hand_entity(h);
                const std::uint32_t held = ctx.hands->held_entity(h);
                if (he != kNone) ignore[n++] = he;
                if (held != kNone && held != self) ignore[n++] = held;
            }
        }
        for (int i = 0; i <= kSamples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSamples);
            const Vec3 from = prev_a_ + (prev_b_ - prev_a_) * t;
            const Vec3 to = a + (b - a) * t;
            Vec3 d = to - from;
            float len = glm::length(d);
            // A blade moving along its own length (a thrust) still needs a
            // test: probe along the blade itself.
            if (len < 0.01f) {
                d = b - a;
                len = glm::length(d);
                if (len < 0.01f) continue;
            }
            PhysicsRayHit h{};
            if (!ctx.physics->sphere_cast_ignoring(ignore, n, from, d / len,
                                                   blunt_ ? face_radius_ * 0.5f : 0.0f,
                                                   len, h) || !h.hit) {
                continue;
            }
            land(world, self, hand, wielder, ctx, h, d / len, speed);
            return;
        }
    }

    void land(World& world, std::uint32_t self, int hand, std::uint32_t wielder,
              MechanicContext& ctx, const PhysicsRayHit& h, const Vec3& dir, float speed) {
        const std::uint32_t target =
            h.entity != kNone ? combat_rules::health_owner(world, h.entity) : kNone;
        const std::uint32_t key = target != kNone ? target : h.entity;
        if (cooldowns_.count(key) != 0) return;
        cooldowns_[key] = cooldown_s_;
        float dealt = 0.0f;
        bool killed = false;
        if (target != kNone && combat_rules::may_damage(ctx.combat, wielder, target)) {
            const float mult = std::clamp(1.0f + speed_scale_ * (speed - min_speed_), 1.0f, 3.0f);
            const float before = ctx.combat->health(target);
            if (ctx.combat->apply_weapon_damage(target, damage_ * mult, wielder)) {
                dealt = std::max(0.0f, before - ctx.combat->health(target));
                killed = !ctx.combat->is_alive(target);
            }
        }
        if (h.entity != kNone && ctx.physics->has_body(h.entity)) {
            ctx.physics->apply_impulse(h.entity, dir * std::clamp(speed * 0.3f, 0.1f, 8.0f));
        }
        const bool organic = target != kNone && world.has<CHealth>(target);
        if (ctx.vfx != nullptr) {
            ImpactStyle style = impact_;
            if (organic && style == ImpactStyle::Sparks) style = blunt_ ? ImpactStyle::Dust
                                                                       : ImpactStyle::Blood;
            ctx.vfx->impact(h.point, h.normal, style, organic ? Vec4(0.6f, 0.03f, 0.03f, 1.0f)
                                                              : Vec4(1.0f, 0.8f, 0.5f, 1.0f));
        }
        if (ctx.haptics != nullptr && hand >= 0) {
            ctx.haptics->pulse(hand, 0.06f, std::clamp(0.35f + 0.08f * speed, 0.3f, 1.0f));
        }
        ++fire_seq_;
        if (ctx.weapons != nullptr) {
            WeaponHit wh{};
            wh.weapon = self;
            wh.shooter = wielder;
            wh.target = target;
            wh.point = h.point;
            wh.normal = h.normal;
            wh.damage = dealt;
            wh.killed = killed;
            wh.delivery = WeaponDelivery::Melee;
            ctx.weapons->publish_hit(wh);
        }
    }

    void publish(MechanicContext& ctx, std::uint32_t self, int hand, std::uint32_t wielder) {
        if (ctx.weapons == nullptr) return;
        WeaponState s{};
        s.configured = true;
        s.delivery = WeaponDelivery::Melee;
        s.infinite = true;
        s.fire_seq = fire_seq_;
        s.held_by_hand = hand;
        s.wielder = wielder;
        ctx.weapons->publish_state(self, s);
    }

    float damage_{35.0f};
    float min_speed_{1.5f};
    float speed_scale_{0.15f};
    float cooldown_s_{0.4f};
    bool blunt_{false};
    ImpactStyle impact_{ImpactStyle::Sparks};
    std::string base_socket_{"strike_base"};
    std::string tip_socket_{"strike_tip"};
    std::string face_socket_{"strike_face"};
    float face_radius_{0.07f};
    bool have_prev_{false};
    Vec3 prev_a_{0.0f};
    Vec3 prev_b_{0.0f};
    std::unordered_map<std::uint32_t, float> cooldowns_;
    std::uint64_t fire_seq_{0};
};

class ShieldMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.shield"; }
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void on_attach(World&, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext& ctx) override {
        blocks_melee_ = p.value("blocks_melee", true);
        leak_ = std::clamp(p.value("leak", 0.0f), 0.0f, 1.0f);
        if (ctx.combat != nullptr && blocks_melee_) ctx.combat->set_blocker(entity, true, leak_);
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World&, std::uint32_t self, float, const InputSnapshot& input,
                   MechanicContext& ctx) override {
        if (ctx.combat == nullptr) return;
        // A block: buzz the hand holding the shield and spark on its face.
        const std::uint64_t seq = ctx.combat->blocked_seq(self);
        if (seq == seen_) return;
        seen_ = seq;
        for (int h = 0; h < 2; ++h) {
            if (input.hands[h].held_entity != self) continue;
            if (ctx.haptics != nullptr) ctx.haptics->pulse(h, 0.08f, 0.8f);
            if (ctx.vfx != nullptr) {
                ctx.vfx->impact(input.hands[h].hand_position, Vec3(0.0f, 1.0f, 0.0f),
                                ImpactStyle::Sparks, Vec4(1.0f, 0.8f, 0.5f, 1.0f));
            }
        }
    }

private:
    bool blocks_melee_{true};
    float leak_{0.0f};
    std::uint64_t seen_{0};
};

}  // namespace

extern "C" int ptv_force_link_builtin_melee = 1;

PTV_REGISTER_MECHANIC("builtin.melee_weapon", MeleeWeaponMechanic)
PTV_REGISTER_MECHANIC("builtin.shield", ShieldMechanic)

}  // namespace ptv
