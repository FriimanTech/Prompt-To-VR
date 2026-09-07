// builtin.switchable / builtin.breakable / builtin.harvestable -- things
// whose state changes without moving.
//
//   * switchable   -- ENUMERATED states, not a boolean. Lights and faucets
//                     are on/off, but a traffic light is green/amber/red and
//                     a conveyor is running/stopped, so the state list comes
//                     from the world builder and this cycles through it. A
//                     boolean would have forced every multi-state object to
//                     encode its state in its name instead.
//   * breakable    -- intact -> destroyed, one way. Pairs with a gated nav
//                     link, so destroying a rock blockage genuinely opens a
//                     route rather than just changing how it looks.
//   * harvestable  -- a REGION that yields an instance when picked. This is
//                     the Quest-safe answer to "the player can pick grass":
//                     one entity for the field plus one spawned blade per
//                     pick, rather than an entity per blade. Measured VR
//                     foliage tests fall under 10 FPS at around 2,000 plant
//                     instances, and Meta's own budget for a busy Quest 3
//                     scene is 200-300 draw calls, so per-blade entities are
//                     not a close call.

#include "AffordanceCommon.h"

#include "ptv/core/Logger.h"

namespace ptv {
namespace {

using namespace ptv::affordance;

/** Shared: resolve my declared id, gate on policy, react to a grip. */
class StateAffordanceBase : public IMechanic {
public:
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext& ctx) override {
        (void)world;
        self_ = entity;
        reach_ = std::clamp(p.value("reach_m", kReach), 0.02f, 3.0f);
        cooldown_s_ = std::clamp(p.value("cooldown_s", 0.25f), 0.0f, 10.0f);
        configure(p);
        id_ = declared_id(ctx, entity);
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t self, float dt,
                   const InputSnapshot& input, MechanicContext& ctx) override {
        if (ctx.interactive == nullptr) return;
        if (id_.empty()) id_ = declared_id(ctx, self);
        if (id_.empty()) return;

        cooldown_ = std::max(0.0f, cooldown_ - dt);

        const auto e = static_cast<entt::entity>(self);
        if (!world.registry().all_of<CTransform>(e)) return;
        const Vec3 here = world.registry().get<CTransform>(e).t.position;

        // The cue is published even during the cooldown, and even while the
        // policy forbids the action, so that it can FADE OUT rather than
        // vanish -- and so a spent switch stops inviting a press.
        const bool allowed = cooldown_ <= 0.0f && actionable(ctx, id_);
        publish_cue(world, self, input, here, allowed, dt, reach_ * 2.0f);
        if (!allowed) return;

        for (int h = 0; h < 2; ++h) {
            if (!grip_started_near(input, h, here, reach_)) continue;
            if (act(world, self, h, ctx)) {
                cooldown_ = cooldown_s_;
                if (ctx.haptics != nullptr) {
                    ctx.haptics->pulse(h, 0.03f, 0.35f);
                }
            }
            break;
        }
    }

protected:
    virtual void configure(const nlohmann::json&) {}
    /** Do the thing. Return false to leave the cooldown untouched, so a
     *  refused action does not feel like a broken one. */
    virtual bool act(World& world, std::uint32_t self, int hand,
                     MechanicContext& ctx) = 0;

    std::uint32_t self_{0xFFFFFFFFu};
    std::string id_;
    float reach_{kReach};
    float cooldown_s_{0.25f};
    float cooldown_{0.0f};
};

/** Enumerated states, cycled on interaction. */
class SwitchableMechanic final : public StateAffordanceBase {
public:
    std::string id() const override { return "builtin.switchable"; }

protected:
    void configure(const nlohmann::json& p) override {
        // The state KEY is configurable because the natural word differs by
        // object: a light is "on", a conveyor is "running", a fireplace is
        // "lit". One key name would have forced every world to lie about one
        // of them.
        key_ = p.value("state_key", std::string("on"));
    }

    bool act(World& world, std::uint32_t self, int, MechanicContext& ctx) override {
        const CInteractive* ci = record(world, self);
        if (ci == nullptr) return false;

        // Several declared positions: cycle. Otherwise: flip the flag.
        if (ci->positions.size() >= 2) {
            const auto current = ctx.interactive->get_state(id_.c_str(), key_.c_str());
            auto it = std::find(ci->positions.begin(), ci->positions.end(),
                                current.name);
            std::size_t next = 0;
            if (it != ci->positions.end()) {
                next = (static_cast<std::size_t>(
                            std::distance(ci->positions.begin(), it)) +
                        1) %
                       ci->positions.size();
            }
            ctx.interactive->set_state(
                id_.c_str(), key_.c_str(),
                InteractiveValue::named(ci->positions[next]));
            return true;
        }

        const bool now =
            ctx.interactive->get_state(id_.c_str(), key_.c_str()).b;
        ctx.interactive->set_state(id_.c_str(), key_.c_str(),
                                   InteractiveValue::of(!now));
        return true;
    }

private:
    std::string key_{"on"};
};

/** intact -> destroyed, one way. */
class BreakableMechanic final : public StateAffordanceBase {
public:
    std::string id() const override { return "builtin.breakable"; }

protected:
    void configure(const nlohmann::json& p) override {
        // A pane of glass breaks when touched; a reinforced wall needs a
        // reason. Hand interaction is therefore opt-in.
        breaks_by_hand_ = p.value("breaks_by_hand", true);
        hide_when_destroyed_ = p.value("hide_when_destroyed", true);
    }

    bool act(World& world, std::uint32_t self, int, MechanicContext& ctx) override {
        if (!breaks_by_hand_) return false;
        if (ctx.interactive->get_state(id_.c_str(), "destroyed").b) {
            return false;  // already gone; nothing to do and no feedback
        }
        break_now(world, self, ctx);
        return true;
    }

    void on_update(World& world, std::uint32_t self, float dt,
                   const InputSnapshot& input, MechanicContext& ctx) override {
        StateAffordanceBase::on_update(world, self, dt, input, ctx);
        // Reflect the state whoever set it -- a projectile, a script, or a
        // restored save -- not just a hand touching it.
        if (ctx.interactive == nullptr || id_.empty()) return;
        const bool destroyed =
            ctx.interactive->get_state(id_.c_str(), "destroyed").b;
        if (destroyed && !applied_) apply_destroyed(world, self, ctx);
        if (!destroyed && applied_) applied_ = false;
    }

private:
    void break_now(World& world, std::uint32_t self, MechanicContext& ctx) {
        ctx.interactive->set_state(id_.c_str(), "destroyed",
                                   InteractiveValue::of(true));
        apply_destroyed(world, self, ctx);
    }

    void apply_destroyed(World& world, std::uint32_t self,
                         MechanicContext& ctx) {
        applied_ = true;
        if (!hide_when_destroyed_) return;
        // Tagging "template" is how the renderer already skips something,
        // so this reuses one existing mechanism rather than adding a second
        // notion of invisibility.
        const auto e = static_cast<entt::entity>(self);
        auto& tag = world.registry().get_or_emplace<CTag>(e);
        if (std::find(tag.tags.begin(), tag.tags.end(), "template") ==
            tag.tags.end()) {
            tag.tags.push_back("template");
        }
        if (ctx.physics != nullptr && ctx.physics->has_body(self)) {
            // Leaving the collider would keep an invisible wall standing
            // where the player just smashed one.
            ctx.physics->set_kinematic(self, true);
        }
    }

    bool breaks_by_hand_{true};
    bool hide_when_destroyed_{true};
    bool applied_{false};
};

/** A region that yields one instance per pick. */
class HarvestableMechanic final : public StateAffordanceBase {
public:
    std::string id() const override { return "builtin.harvestable"; }

protected:
    void configure(const nlohmann::json& p) override {
        // 0 = unlimited. A flower bed regrows; a one-off cache does not.
        max_picks_ = std::max(0, p.value("max_picks", 0));
        // Picking is a reach-and-squeeze over an AREA, so the default reach
        // is wider than a doorknob's.
        reach_ = std::clamp(p.value("reach_m", 0.5f), 0.05f, 5.0f);
        into_hand_ = p.value("into_hand", true);
    }

    bool act(World& world, std::uint32_t self, int hand,
             MechanicContext& ctx) override {
        const CInteractive* ci = record(world, self);
        if (ci == nullptr || ci->yields_asset_id.empty()) return false;
        if (ctx.spawn == nullptr || ctx.hands == nullptr) return false;

        const float picked =
            ctx.interactive->get_state(id_.c_str(), "picked").number;
        if (max_picks_ > 0 && picked >= static_cast<float>(max_picks_)) {
            return false;
        }

        // The yield is a scene TEMPLATE, cloned the same way every runtime
        // spawn works, so the blade arrives with its collider, tags and
        // mechanic bindings already correct.
        const auto tmpl = world.find(ci->yields_asset_id);
        if (tmpl == entt::null) {
            PTV_WARN(
                "builtin.harvestable '{}' yields '{}', but no template entity "
                "with that id exists in the scene",
                id_, ci->yields_asset_id);
            return false;
        }

        const auto& hs = ctx.hands;
        Vec3 at = world.registry().all_of<CTransform>(
                      static_cast<entt::entity>(self))
                      ? world.registry()
                            .get<CTransform>(static_cast<entt::entity>(self))
                            .t.position
                      : Vec3(0.0f);
        (void)hs;

        const std::uint32_t made = ctx.spawn->instance(
            static_cast<std::uint32_t>(tmpl), at,
            Quat(1.0f, 0.0f, 0.0f, 0.0f));
        if (made == InputSnapshot::kInvalidEntity) return false;

        if (into_hand_ && ctx.hands->held_entity(hand) ==
                              InputSnapshot::kInvalidEntity) {
            ctx.hands->set_held_entity(hand, made);
        }

        ctx.interactive->set_state(id_.c_str(), "picked",
                                   InteractiveValue::of(picked + 1.0f));
        return true;
    }

private:
    int max_picks_{0};
    bool into_hand_{true};
};

}  // namespace

// Anchor so MSVC keeps this TU (and its registrations) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_state_affordances = 1;

PTV_REGISTER_MECHANIC("builtin.switchable", SwitchableMechanic)
PTV_REGISTER_MECHANIC("builtin.breakable", BreakableMechanic)
PTV_REGISTER_MECHANIC("builtin.harvestable", HarvestableMechanic)

}  // namespace ptv
