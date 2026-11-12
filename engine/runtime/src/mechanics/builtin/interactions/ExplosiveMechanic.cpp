// builtin.explosive -- a grenade, flashbang or other explosive the player
// arms and throws.
//
// The shooting range had a generated "activation and throwing" hub that ran
// a fuse and nothing else: the frag grenade never exploded and nothing it
// was thrown at broke. Like weapons, explosives are the engine's: the
// interaction design says what it is (frag, flash, smoke, impact), this runs
// it, and damage goes through ctx.combat so a grenade breaks a
// builtin.destructible target or kills an NPC from any other task.
//
//   * Arm: pull the trigger while holding it (the pin) -- a click and a tick
//     of haptics. ``on_impact`` explosives need no arming.
//   * Fuse: starts when an armed explosive LEAVES the hand (the spoon flies),
//     so it never goes off in a hand that is still holding it. Thrown via
//     builtin.grab's release velocity.
//   * Detonate:
//       frag   -- damage to everything with health inside ``radius_m``
//                 (full inside ``inner_radius_m``, falling to 0 at the edge,
//                 blocked by walls), an impulse on loose props, a fireball,
//                 light and a scorch decal.
//       flash  -- a white-out for the player scaled by distance and by how
//                 directly they were looking, decaying over ``stun_s``; NPCs
//                 in sight are stunned (CStun) for ``stun_s``.
//       smoke  -- a lingering dust cloud.
//       impact -- detonates (like frag) on the first hit after the throw.
//   * Restock: a scene-placed explosive reappears where it started after
//     ``respawn_after_s`` (4 s), so a range table never runs dry; a spawned
//     copy is despawned.
//
// Params: kind, activation, fuse_s, radius_m, inner_radius_m, damage,
// impulse, flash_radius_m, stun_s, hurts_player, respawn_after_s.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/Effects.h"
#include "ptv/api/GameFlow.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

void set_tag(World& world, std::uint32_t e, const char* tag, bool on) {
    auto& tags = world.registry().get_or_emplace<CTag>(static_cast<entt::entity>(e)).tags;
    const auto it = std::find(tags.begin(), tags.end(), tag);
    if (on && it == tags.end()) tags.push_back(tag);
    if (!on && it != tags.end()) tags.erase(it);
}

class ExplosiveMechanic final : public BuiltinMechanicBase<ExplosiveMechanic> {
public:
    static constexpr const char* kId = "builtin.explosive";
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json&) {}

    void on_attach(World& world, std::uint32_t self, const nlohmann::json& p,
                   MechanicContext&) override {
        kind_ = p.value("kind", std::string("frag"));
        activation_ = p.value("activation", std::string("pull_pin_trigger"));
        fuse_s_ = std::clamp(p.value("fuse_s", 3.0f), 0.0f, 30.0f);
        radius_ = std::clamp(p.value("radius_m", 6.0f), 0.5f, 100.0f);
        inner_ = std::clamp(p.value("inner_radius_m", 1.5f), 0.0f, radius_);
        damage_ = std::max(0.0f, p.value("damage", kind_ == "frag" ? 120.0f : 0.0f));
        impulse_ = std::max(0.0f, p.value("impulse", 12.0f));
        flash_radius_ = std::clamp(p.value("flash_radius_m", 10.0f), 0.0f, 100.0f);
        stun_s_ = std::clamp(p.value("stun_s", 4.0f), 0.0f, 60.0f);
        hurts_player_ = p.value("hurts_player", false);
        respawn_s_ = std::clamp(p.value("respawn_after_s", 4.0f), 0.0f, 600.0f);
        if (world.has<CTransform>(self)) home_ = world.get<CTransform>(self).t;
        const std::string* id = world.id_of(static_cast<entt::entity>(self));
        name_ = id != nullptr ? *id : std::string("explosive");
        spawned_ = id == nullptr;
        state_ = State::Idle;
    }

    void tick(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (!world.valid(self) || !world.has<CTransform>(self)) return;
        const int hand = holder(ctx, self);
        const Vec3 pos = world.get<CTransform>(self).t.position;
        switch (state_) {
            case State::Idle:
                if (activation_ == "on_impact") {
                    if (hand >= 0) state_ = State::Armed;
                    break;
                }
                if (hand >= 0) {
                    const auto& hs = input.hands[static_cast<std::size_t>(hand)];
                    if (hs.prev_trigger < 0.7f && hs.trigger >= 0.7f) arm(ctx, hand);
                }
                break;
            case State::Armed:
                if (hand < 0) {  // released: the spoon flies
                    state_ = State::Fused;
                    timer_ = fuse_s_;
                    last_pos_ = pos;
                    thrown_speed_ = 0.0f;
                    PTV_INFO("explosive: '{}' thrown, fuse {:.1f} s", name_, fuse_s_);
                }
                break;
            case State::Fused: {
                timer_ -= dt;
                const float speed = glm::length(pos - last_pos_) / std::max(dt, 1e-4f);
                thrown_speed_ = std::max(thrown_speed_, speed);
                last_pos_ = pos;
                const bool impact = activation_ == "on_impact" && thrown_speed_ > 2.0f &&
                                    speed < 0.3f * thrown_speed_;
                if (ctx.vfx != nullptr && kind_ != "smoke") {
                    ctx.vfx->glow(pos, 0.03f, Vec4(1.0f, 0.3f, 0.1f, 1.0f));
                }
                if (hand >= 0 && activation_ == "on_impact") {
                    state_ = State::Armed;  // caught again
                    break;
                }
                if (timer_ <= 0.0f || impact) detonate(world, self, ctx, input, pos);
                break;
            }
            case State::Spent:
                after(world, self, ctx, input, dt);
                break;
        }
    }

private:
    enum class State { Idle, Armed, Fused, Spent };

    int holder(MechanicContext& ctx, std::uint32_t self) const {
        if (ctx.hands == nullptr) return -1;
        for (int h = 0; h < 2; ++h) {
            if (ctx.hands->held_entity(h) == self) return h;
        }
        return -1;
    }

    void arm(MechanicContext& ctx, int hand) {
        state_ = State::Armed;
        if (ctx.haptics != nullptr) ctx.haptics->pulse_at(hand, 0.04f, 260.0f, 0.6f);
        PTV_INFO("explosive: '{}' armed ({} hand)", name_, hand == 0 ? "left" : "right");
    }

    void detonate(World& world, std::uint32_t self, MechanicContext& ctx,
                  const InputSnapshot& input, const Vec3& pos) {
        state_ = State::Spent;
        Vec3 centre = pos;
        if (ctx.physics != nullptr) ctx.physics->body_center(self, &centre);
        const std::uint32_t player = ctx.hands != nullptr ? ctx.hands->player_entity() : kNone;
        int hits = 0;
        if (kind_ == "frag" || kind_ == "impact" || kind_ == "sticky") {
            hits = blast(world, self, ctx, centre, player);
            if (ctx.vfx != nullptr) {
                ctx.vfx->explosion(centre, std::max(0.6f, 0.3f * radius_),
                                   Vec4(1.0f, 0.55f, 0.18f, 1.0f));
                ctx.vfx->light_flash(centre, Vec3(1.0f, 0.6f, 0.3f), 6.0f, radius_ * 2.0f, 0.35f);
                ctx.vfx->decal(Vec3(centre.x, centre.y - 0.05f, centre.z), Vec3(0.0f, 1.0f, 0.0f),
                               0.9f, Vec4(0.05f, 0.05f, 0.05f, 0.8f));
            }
            rumble(ctx, input, centre, 0.9f);
        } else if (kind_ == "flash") {
            hits = stun(world, self, ctx, centre, player);
            flash_total_ = 0.0f;
            if (input.head_valid) {
                const Vec3 to = centre - Vec3(input.head_position);
                const float d = glm::length(to);
                if (d < flash_radius_) {
                    const Vec3 fwd = Vec3(input.head_rotation * Vec3(0.0f, 0.0f, -1.0f));
                    const float look = d > 1e-3f ? glm::dot(fwd, to / d) : 1.0f;
                    flash_total_ = (1.0f - d / flash_radius_) *
                                   std::clamp(0.45f + 0.55f * look, 0.25f, 1.0f);
                }
            }
            flash_left_ = stun_s_ * std::max(flash_total_, 0.25f);
            if (ctx.vfx != nullptr) {
                ctx.vfx->light_flash(centre, Vec3(1.0f), 12.0f, flash_radius_ * 2.0f, 0.25f);
                ctx.vfx->explosion(centre, 0.5f, Vec4(1.0f, 1.0f, 0.95f, 1.0f));
            }
            rumble(ctx, input, centre, 0.6f);
        } else {  // smoke
            smoke_left_ = 8.0f;
        }
        set_tag(world, self, "template", true);
        if (ctx.physics != nullptr) ctx.physics->set_collision_enabled(self, false);
        respawn_timer_ = respawn_s_;
        if (ctx.signals != nullptr) {
            ctx.signals->emit((name_ + ".detonated").c_str());
            ctx.signals->emit("explosive.detonated");
        }
        PTV_INFO("explosive: '{}' detonated kind={} at {:.2f},{:.2f},{:.2f} hits={}", name_,
                 kind_, centre.x, centre.y, centre.z, hits);
    }

    /** Radial damage with falloff and wall occlusion, plus an impulse on loose props. */
    int blast(World& world, std::uint32_t self, MechanicContext& ctx, const Vec3& c,
              std::uint32_t player) {
        int hits = 0;
        std::vector<std::uint32_t> targets;
        auto view = world.registry().view<CTransform, CHealth>();
        for (auto e : view) {
            const auto id = static_cast<std::uint32_t>(e);
            if (id == self || !view.get<CHealth>(e).alive) continue;
            if (id == player && !hurts_player_) continue;
            targets.push_back(id);
        }
        for (const std::uint32_t t : targets) {
            Vec3 tc = world.get<CTransform>(t).t.position;
            if (ctx.physics != nullptr) ctx.physics->body_center(t, &tc);
            const float d = glm::length(tc - c);
            if (d > radius_) continue;
            if (!clear_line(ctx, self, t, c, tc)) continue;
            const float k = d <= inner_ ? 1.0f : 1.0f - (d - inner_) / std::max(1e-3f, radius_ - inner_);
            if (ctx.combat != nullptr &&
                ctx.combat->apply_weapon_damage(t, damage_ * k, player)) {
                ++hits;
            }
        }
        if (ctx.physics != nullptr && impulse_ > 0.0f) {
            auto bodies = world.registry().view<CTransform, CPhysics>();
            for (auto e : bodies) {
                const auto id = static_cast<std::uint32_t>(e);
                if (id == self || bodies.get<CPhysics>(e).kind != "dynamic") continue;
                const Vec3 bp = bodies.get<CTransform>(e).t.position;
                const Vec3 dvec = bp - c;
                const float d = glm::length(dvec);
                if (d > radius_ || d < 1e-3f) continue;
                const Vec3 dir = glm::normalize(dvec + Vec3(0.0f, 0.3f * d, 0.0f));
                ctx.physics->apply_impulse(id, dir * impulse_ * (1.0f - d / radius_));
            }
        }
        return hits;
    }

    int stun(World& world, std::uint32_t self, MechanicContext& ctx, const Vec3& c,
             std::uint32_t player) {
        int n = 0;
        auto view = world.registry().view<CTransform, CHealth>();
        std::vector<std::uint32_t> victims;
        for (auto e : view) {
            const auto id = static_cast<std::uint32_t>(e);
            if (id == self || id == player || !view.get<CHealth>(e).alive) continue;
            if (!world.has<CFaction>(id) && !world.has<CLocomotionIntent>(id)) continue;
            victims.push_back(id);
        }
        for (const std::uint32_t v : victims) {
            Vec3 vc = world.get<CTransform>(v).t.position;
            if (ctx.physics != nullptr) ctx.physics->body_center(v, &vc);
            if (glm::length(vc - c) > flash_radius_ || !clear_line(ctx, self, v, c, vc)) continue;
            world.registry().emplace_or_replace<CStun>(static_cast<entt::entity>(v),
                                                       CStun{stun_s_});
            ++n;
        }
        return n;
    }

    /** Only level geometry -- walls, floors -- shields a target from the
     *  blast; props and bodies in the way do not. */
    static bool clear_line(MechanicContext& ctx, std::uint32_t self, std::uint32_t, const Vec3& a,
                           const Vec3& b) {
        if (ctx.physics == nullptr) return true;
        const Vec3 d = b - a;
        const float len = glm::length(d);
        if (len < 0.15f) return true;
        PhysicsRayHit hit{};
        return !ctx.physics->raycast_floor(self, a, d / len, len - 0.1f, hit);
    }

    void rumble(MechanicContext& ctx, const InputSnapshot& input, const Vec3& c, float amp) {
        if (ctx.haptics == nullptr || !input.head_valid) return;
        const float d = glm::length(c - Vec3(input.head_position));
        const float k = std::clamp(1.0f - d / (radius_ * 2.5f), 0.0f, 1.0f);
        if (k <= 0.0f) return;
        for (int h = 0; h < 2; ++h) ctx.haptics->pulse_at(h, 0.25f, 70.0f, amp * k);
    }

    void after(World& world, std::uint32_t self, MechanicContext& ctx, const InputSnapshot&,
               float dt) {
        if (flash_left_ > 0.0f) {
            flash_left_ -= dt;
            const float total = stun_s_ * std::max(flash_total_, 0.25f);
            const float a = flash_total_ * std::clamp(flash_left_ / std::max(total, 1e-3f), 0.0f, 1.0f);
            if (ctx.app != nullptr && a > 0.0f) ctx.app->request_flash(std::min(1.0f, 1.2f * a), Vec3(1.0f));
        }
        if (smoke_left_ > 0.0f) {
            smoke_left_ -= dt;
            if (ctx.vfx != nullptr && std::fmod(smoke_left_, 0.4f) < dt) {
                ctx.vfx->impact(home_.position, Vec3(0.0f, 1.0f, 0.0f), ImpactStyle::Dust,
                                Vec4(0.7f, 0.7f, 0.72f, 0.8f));
            }
        }
        respawn_timer_ -= dt;
        if (respawn_timer_ > 0.0f || flash_left_ > 0.0f) return;
        if (spawned_ || respawn_s_ <= 0.0f) {
            if (ctx.spawn != nullptr) ctx.spawn->despawn(self);
            return;
        }
        // Restock: back where the scene put it.
        set_tag(world, self, "template", false);
        if (ctx.physics != nullptr) {
            ctx.physics->set_collision_enabled(self, true);
            ctx.physics->teleport_body(self, home_.position, home_.rotation);
        }
        world.get<CTransform>(self).t = home_;
        state_ = State::Idle;
        PTV_INFO("explosive: '{}' restocked", name_);
    }

    std::string kind_{"frag"};
    std::string activation_{"pull_pin_trigger"};
    std::string name_;
    Transform home_{};
    State state_{State::Idle};
    Vec3 last_pos_{0.0f};
    float fuse_s_{3.0f};
    float radius_{6.0f};
    float inner_{1.5f};
    float damage_{120.0f};
    float impulse_{12.0f};
    float flash_radius_{10.0f};
    float stun_s_{4.0f};
    float respawn_s_{4.0f};
    float timer_{0.0f};
    float thrown_speed_{0.0f};
    float flash_total_{0.0f};
    float flash_left_{0.0f};
    float smoke_left_{0.0f};
    float respawn_timer_{0.0f};
    bool hurts_player_{false};
    bool spawned_{false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_explosive = 1;

PTV_REGISTER_MECHANIC("builtin.explosive", ExplosiveMechanic)

}  // namespace ptv
