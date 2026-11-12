// builtin.npc.melee_chase -- an enemy that runs you down and hits you.
//
// Chase + telegraphed melee attack + hurt reaction is most of what enemies in
// generated games do, and the per-game versions kept getting the same things
// wrong: no wind-up (damage lands the instant the enemy arrives, which reads
// as unfair), damage applied through a wall, a hurt reaction that never ends,
// or a dead enemy still swinging. Every number is a param, so a standard,
// a tall-and-fast and a big-and-bulky variant of the same family are the same
// mechanic with different settings.
//
// It writes a CLocomotionIntent and plays one-shot clips; movement, gait
// clips and death belong to the engine.
//
// Params:
//   target_tag          "" = nearest hostile (faction), else a tag ("player")
//   sight_range_m       notice targets this close (30)
//   chase_speed_mps     (3.5)    walk_speed_mps (1.4) when the target is near
//   sprint              prefer the run clip while chasing (true)
//   attack_range_m      start an attack inside this (1.6)
//   windup_s            telegraph before the hit lands (0.5)
//   attack_cooldown_s   between attacks (1.5)
//   damage              per hit (10)
//   hit_reach_m         the hit lands if still this close at the end of the
//                       wind-up (attack_range_m + 0.6)
//   stagger_s           stand still this long after being hurt (0.35)
//   attack_clip / hurt_clip  intents to play ("attack" / "hurt")
//   gait_variant_when_chasing  e.g. "punching" (optional)

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/Facing.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

class NpcMeleeChaseMechanic final
    : public BuiltinMechanicBase<NpcMeleeChaseMechanic> {
public:
    static constexpr const char* kId = "builtin.npc.melee_chase";

    void configure(const nlohmann::json& p) {
        target_tag_ = p.value("target_tag", std::string());
        sight_ = std::clamp(p.value("sight_range_m", 30.0f), 1.0f, 1000.0f);
        chase_ = std::clamp(p.value("chase_speed_mps", 3.5f), 0.1f, 40.0f);
        walk_ = std::clamp(p.value("walk_speed_mps", 1.4f), 0.1f, chase_);
        sprint_ = p.value("sprint", true);
        range_ = std::clamp(p.value("attack_range_m", 1.6f), 0.3f, 50.0f);
        windup_ = std::clamp(p.value("windup_s", 0.5f), 0.0f, 10.0f);
        cooldown_ = std::clamp(p.value("attack_cooldown_s", 1.5f), 0.05f, 60.0f);
        damage_ = std::clamp(p.value("damage", 10.0f), 0.0f, 100000.0f);
        reach_ = std::clamp(p.value("hit_reach_m", range_ + 0.6f), range_, 60.0f);
        stagger_ = std::clamp(p.value("stagger_s", 0.35f), 0.0f, 10.0f);
        attack_clip_ = p.value("attack_clip", std::string("attack"));
        hurt_clip_ = p.value("hurt_clip", std::string("hurt"));
        chase_variant_ = p.value("gait_variant_when_chasing", std::string());
    }

    void tick(World& world, std::uint32_t self, float dt, const InputSnapshot&,
              MechanicContext& ctx) {
        if (!world.valid(self) || !world.has<CTransform>(self)) return;
        auto& want = world.registry().get_or_emplace<CLocomotionIntent>(
            static_cast<entt::entity>(self));
        // Stunned (a flashbang): stand still, attack nothing.
        if (world.has<CStun>(self)) {
            want.desired_velocity = Vec3(0.0f);
            return;
        }
        if (!alive(world, ctx, self)) {
            want.desired_velocity = Vec3(0.0f);
            return;
        }

        cooldown_t_ = std::max(0.0f, cooldown_t_ - dt);
        stagger_t_ = std::max(0.0f, stagger_t_ - dt);

        // Hurt: health went down since last frame.
        if (world.has<CHealth>(self)) {
            const float hp = world.get<CHealth>(self).current;
            if (last_hp_ >= 0.0f && hp < last_hp_ - 1e-4f) {
                stagger_t_ = stagger_;
                windup_t_ = -1.0f;  // a hit interrupts the swing
                if (ctx.skeleton != nullptr && !hurt_clip_.empty()) {
                    ctx.skeleton->play_clip(self, hurt_clip_.c_str(), false, 0.08f);
                }
            }
            last_hp_ = hp;
        }

        const std::uint32_t target = find_target(world, ctx, self);
        want.gait_variant.clear();
        if (target == kInvalid || stagger_t_ > 0.0f) {
            want.desired_velocity = Vec3(0.0f);
            want.face_travel = true;
            want.has_face_target = false;
            want.has_look_target = false;
            return;
        }

        const Vec3 me = world.get<CTransform>(self).t.position;
        const Vec3 them = target_position(world, ctx, target);
        Vec3 flat = them - me;
        flat.y = 0.0f;
        // Reach is horizontal, but only on roughly the same level: a target
        // on a balcony above, or one standing on the roof the NPC is under,
        // is out of reach however close it is in plan.
        const bool same_level = std::fabs(them.y - me.y) <= kMaxReachHeightM;
        const float d = same_level ? glm::length(flat)
                                   : std::max(glm::length(flat), reach_ + 1.0f);

        // Mid-swing: hold still, keep facing, land the hit at the end.
        if (windup_t_ >= 0.0f) {
            want.desired_velocity = Vec3(0.0f);
            want.face_travel = false;
            want.has_face_target = true;
            want.face_target = them;
            windup_t_ -= dt;
            if (windup_t_ < 0.0f) {
                if (d <= reach_ && clear_line(ctx, self, target)) {
                    if (ctx.combat != nullptr) {
                        ctx.combat->apply_damage(target, damage_, self);
                    } else if (world.has<CHealth>(target)) {
                        auto& hp = world.get<CHealth>(target);
                        hp.current = std::max(0.0f, hp.current - damage_);
                    }
                }
                cooldown_t_ = cooldown_;
            }
            return;
        }

        // The head tracks the target whatever the body is doing.
        want.has_look_target = true;
        want.look_target = them;

        if (d <= range_) {
            want.desired_velocity = Vec3(0.0f);
            want.face_travel = false;
            want.has_face_target = true;
            want.face_target = them;
            // Never swing at something behind it: wait until the body has
            // turned to face the target, so the attack visibly lands. A body
            // that cannot turn (nothing drives its locomotion) still attacks
            // after a moment rather than standing there forever.
            const float err = glm::degrees(heading_error(
                facing_of(world.get<CTransform>(self).t.rotation), flat));
            unfaced_t_ = err <= kMaxAttackFacingErrDeg ? 0.0f : unfaced_t_ + dt;
            const bool faced =
                err <= kMaxAttackFacingErrDeg || unfaced_t_ >= kMaxTurnWaitS;
            if (cooldown_t_ <= 0.0f && faced) {
                windup_t_ = windup_;
                if (ctx.skeleton != nullptr && !attack_clip_.empty()) {
                    ctx.skeleton->play_clip(self, attack_clip_.c_str(), false, 0.08f);
                }
            }
            return;
        }

        // Chase. next_waypoint routes around walls and through doorways and
        // keeps siblings apart.
        Vec3 wp = them;
        if (ctx.nav != nullptr) wp = ctx.nav->next_waypoint(self, them);
        Vec3 dir = wp - me;
        dir.y = 0.0f;
        if (glm::length(dir) < 1e-4f) dir = flat;
        const bool close = d < range_ * 3.0f;
        // Far away it faces where it is going (around corners, through
        // doors). Closing in it squares up to the target, so it arrives
        // already facing what it is about to hit.
        want.face_travel = !close;
        want.has_face_target = close;
        want.face_target = them;
        want.desired_velocity =
            glm::length(dir) > 1e-4f ? glm::normalize(dir) * (close ? walk_ : chase_)
                                     : Vec3(0.0f);
        want.sprint = sprint_ && !close;
        if (!close && !chase_variant_.empty()) want.gait_variant = chase_variant_;
    }

private:
    bool alive(World& world, MechanicContext& ctx, std::uint32_t e) const {
        if (ctx.combat != nullptr) return ctx.combat->is_alive(e);
        return !world.has<CHealth>(e) || world.get<CHealth>(e).alive;
    }

    std::uint32_t find_target(World& world, MechanicContext& ctx,
                              std::uint32_t self) const {
        if (target_tag_.empty()) {
            if (ctx.perception != nullptr) {
                return ctx.perception->nearest_hostile(self, sight_);
            }
            return find_tagged(world, ctx, self, "player");
        }
        if (target_tag_ == "player" && ctx.hands != nullptr) {
            const std::uint32_t p = ctx.hands->player_entity();
            if (p != kInvalid && world.valid(p) && alive(world, ctx, p) &&
                in_sight(world, ctx, self, p)) {
                return p;
            }
            return kInvalid;
        }
        return find_tagged(world, ctx, self, target_tag_);
    }

    std::uint32_t find_tagged(World& world, MechanicContext& ctx, std::uint32_t self,
                              const std::string& tag) const {
        std::uint32_t best = kInvalid;
        float best_d = sight_;
        const Vec3 me = world.get<CTransform>(self).t.position;
        auto view = world.registry().view<const CTag, const CTransform>();
        for (const auto e : view) {
            const auto id = static_cast<std::uint32_t>(e);
            if (id == self) continue;
            bool match = false;
            for (const auto& t : view.get<const CTag>(e).tags) {
                if (t == tag) match = true;
                if (t == "template") {
                    match = false;
                    break;
                }
            }
            if (!match || !alive(world, ctx, id)) continue;
            const float d = glm::length(view.get<const CTransform>(e).t.position - me);
            if (d < best_d) {
                best_d = d;
                best = id;
            }
        }
        return best;
    }

    bool in_sight(World& world, MechanicContext& ctx, std::uint32_t self,
                  std::uint32_t target) const {
        const Vec3 d = target_position(world, ctx, target) -
                       world.get<CTransform>(self).t.position;
        return glm::length(Vec3(d.x, 0.0f, d.z)) <= sight_;
    }

    Vec3 target_position(World& world, MechanicContext& ctx,
                         std::uint32_t target) const {
        if (ctx.hands != nullptr && target == ctx.hands->player_entity() &&
            ctx.locomotion != nullptr) {
            return ctx.locomotion->body_position();
        }
        return world.get<CTransform>(target).t.position;
    }

    bool clear_line(MechanicContext& ctx, std::uint32_t self,
                    std::uint32_t target) const {
        if (ctx.perception == nullptr) return true;
        return ctx.perception->line_of_sight(self, target);
    }

    static constexpr float kMaxReachHeightM = 2.0f;
    static constexpr float kMaxAttackFacingErrDeg = 35.0f;
    static constexpr float kMaxTurnWaitS = 1.0f;
    float unfaced_t_{0.0f};

    std::string target_tag_;
    float sight_{30.0f}, chase_{3.5f}, walk_{1.4f};
    bool sprint_{true};
    float range_{1.6f}, windup_{0.5f}, cooldown_{1.5f}, damage_{10.0f};
    float reach_{2.2f}, stagger_{0.35f};
    std::string attack_clip_{"attack"}, hurt_clip_{"hurt"}, chase_variant_;
    float cooldown_t_{0.0f};
    float stagger_t_{0.0f};
    float windup_t_{-1.0f};
    float last_hp_{-1.0f};
};

}  // namespace

extern "C" int ptv_force_link_builtin_npc_melee_chase = 1;

PTV_REGISTER_MECHANIC("builtin.npc.melee_chase", NpcMeleeChaseMechanic)

}  // namespace ptv
