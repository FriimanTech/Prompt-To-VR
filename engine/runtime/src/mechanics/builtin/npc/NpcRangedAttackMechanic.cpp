// builtin.npc.ranged_attack -- an NPC that shoots with the weapon it carries.
//
// Executes the NpcPlan's ``ranged_attack`` behaviour. The NPC finds the
// nearest hostile it can see, keeps a firing distance (closing in beyond
// ``preferred_range_m``, backing off inside ``min_range_m``), faces the target
// through CLocomotionIntent (builtin.character_locomotion moves and animates
// the body -- this never writes CTransform), and fires the weapon attached to
// it in bursts through ``ctx.weapons``: trigger, aim point, pause. The weapon
// itself (builtin.weapon) does the shooting, the ammo and the damage, so an
// NPC's rifle and the player's rifle behave the same.
//
// Params: weapon (entity id; default: the first entity attached to this NPC
// that has weapon state, else the NPC itself), preferred_range_m (8), min_range_m (4), sight_m (25),
// move_speed_mps (2.5), burst (3 shots), burst_pause_s (1.2), reaction_s
// (0.5), aim_error_m (0.25: how far off the aim point wanders).

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "../../../gameplay/CombatRules.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;

class NpcRangedAttackMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.npc.ranged_attack"; }
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void on_attach(World& world, std::uint32_t, const nlohmann::json& p, MechanicContext&) override {
        preferred_ = std::clamp(p.value("preferred_range_m", 8.0f), 1.0f, 200.0f);
        min_range_ = std::clamp(p.value("min_range_m", 4.0f), 0.0f, preferred_);
        sight_ = std::clamp(p.value("sight_m", 25.0f), 2.0f, 500.0f);
        speed_ = std::clamp(p.value("move_speed_mps", 2.5f), 0.0f, 20.0f);
        burst_ = std::clamp(p.value("burst", 3), 1, 50);
        pause_s_ = std::clamp(p.value("burst_pause_s", 1.2f), 0.0f, 20.0f);
        reaction_s_ = std::clamp(p.value("reaction_s", 0.5f), 0.0f, 10.0f);
        aim_error_ = std::clamp(p.value("aim_error_m", 0.25f), 0.0f, 5.0f);
        const std::string w = p.value("weapon", std::string());
        if (!w.empty()) {
            const auto e = world.find(w);
            if (e != entt::null) weapon_ = static_cast<std::uint32_t>(e);
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t self, float dt, const InputSnapshot&,
                   MechanicContext& ctx) override {
        if (ctx.weapons == nullptr || ctx.perception == nullptr) return;
        if (weapon_ == kNone || !world.valid(weapon_)) weapon_ = find_weapon(world, self, ctx);
        if (ctx.combat != nullptr && !ctx.combat->is_alive(self)) {
            stop(ctx);
            return;
        }
        const bool live = ctx.game == nullptr || ctx.game->gameplay_active();
        const std::uint32_t target = live ? ctx.perception->nearest_hostile(self, sight_) : kNone;
        auto& want = world.registry().get_or_emplace<CLocomotionIntent>(static_cast<entt::entity>(self));
        if (target == kNone || !world.has<CTransform>(target) ||
            !ctx.perception->line_of_sight(self, target)) {
            seen_s_ = 0.0f;
            want.desired_velocity = Vec3(0.0f);
            want.face_travel = true;
            want.has_face_target = false;
            stop(ctx);
            return;
        }
        const Vec3 me = world.get<CTransform>(self).t.position;
        Vec3 aim = combat_rules::aim_point(world, target, ctx.physics);
        if (ctx.hands != nullptr && target == ctx.hands->player_entity() && ctx.locomotion != nullptr) {
            aim = ctx.locomotion->head_position() - Vec3(0.0f, 0.3f, 0.0f);
        }
        Vec3 to = aim - me;
        to.y = 0.0f;
        const float dist = glm::length(to);
        const Vec3 dir = dist > 1e-4f ? to / dist : Vec3(0.0f, 0.0f, -1.0f);
        if (dist > preferred_) {
            want.desired_velocity = dir * speed_;
        } else if (dist < min_range_) {
            want.desired_velocity = -dir * speed_ * 0.6f;
        } else {
            want.desired_velocity = Vec3(0.0f);
        }
        want.face_travel = false;
        want.face_target = aim;
        want.has_face_target = true;

        seen_s_ += dt;
        if (weapon_ == kNone || seen_s_ < reaction_s_) return;
        // Bursts: hold the trigger for ``burst_`` shots, then pause.
        WeaponState ws{};
        ctx.weapons->state(weapon_, &ws);
        wobble_t_ += dt;
        const Vec3 err(std::sin(wobble_t_ * 3.1f), std::sin(wobble_t_ * 2.3f + 1.0f),
                       std::cos(wobble_t_ * 2.7f));
        ctx.weapons->aim_at(weapon_, aim + err * aim_error_, true);
        if (pause_left_ > 0.0f) {
            pause_left_ -= dt;
            ctx.weapons->trigger(weapon_, false);
            return;
        }
        if (!firing_) {
            firing_ = true;
            burst_start_seq_ = ws.fire_seq;
        }
        ctx.weapons->trigger(weapon_, true);
        if (ws.fire_seq - burst_start_seq_ >= static_cast<std::uint64_t>(burst_) ||
            (ws.reloading)) {
            firing_ = false;
            pause_left_ = pause_s_;
            ctx.weapons->trigger(weapon_, false);
        }
    }

private:
    std::uint32_t find_weapon(World& world, std::uint32_t self, MechanicContext& ctx) const {
        auto view = world.registry().view<const CAttachment>();
        for (auto e : view) {
            if (view.get<const CAttachment>(e).parent != self) continue;
            const auto id = static_cast<std::uint32_t>(e);
            if (ctx.weapons->state(id, nullptr)) return id;
        }
        // A character modelled holding its gun carries builtin.weapon itself.
        if (ctx.weapons->state(self, nullptr)) return self;
        return kNone;
    }

    void stop(MechanicContext& ctx) {
        firing_ = false;
        if (weapon_ != kNone) {
            ctx.weapons->trigger(weapon_, false);
            ctx.weapons->aim_at(weapon_, Vec3(0.0f), false);
        }
    }

    float preferred_{8.0f};
    float min_range_{4.0f};
    float sight_{25.0f};
    float speed_{2.5f};
    int burst_{3};
    float pause_s_{1.2f};
    float reaction_s_{0.5f};
    float aim_error_{0.25f};
    std::uint32_t weapon_{kNone};
    float seen_s_{0.0f};
    float pause_left_{0.0f};
    bool firing_{false};
    std::uint64_t burst_start_seq_{0};
    float wobble_t_{0.0f};
};

}  // namespace

extern "C" int ptv_force_link_builtin_npc_ranged = 1;

PTV_REGISTER_MECHANIC("builtin.npc.ranged_attack", NpcRangedAttackMechanic)

}  // namespace ptv
