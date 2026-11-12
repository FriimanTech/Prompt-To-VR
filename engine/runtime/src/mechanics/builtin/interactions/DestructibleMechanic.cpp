// builtin.destructible -- something shot or blown up that BREAKS: a range
// target, a crate, a bottle, a barrel.
//
// The shooting range had two half-answers that disagreed: the world declared
// its targets as ``breakable`` interactives with a state key the breakable
// built-in never read ("broken", not "destroyed"), and gameplay gave its own
// copies hit points nothing acted on. A destructible is simply this: it has
// health (CHealth), every kind of damage lowers it -- bullets, beams, melee,
// a grenade's blast through ctx.combat -- and when it runs out the object
// breaks with an effect, loses its collider, raises ``<id>.destroyed``, sets
// its interactive ``destroyed`` state if it has one, and optionally comes
// back after ``respawn_after_s``.
//
// Params: effect (shatter | explode | knock_down | vanish), respawn_after_s
// (0 = never), max_health (when the entity has no health block yet).

#include <algorithm>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/Effects.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

void add_tag(World& world, std::uint32_t e, const char* tag) {
    auto& tags = world.registry().get_or_emplace<CTag>(static_cast<entt::entity>(e)).tags;
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
}

void remove_tag(World& world, std::uint32_t e, const char* tag) {
    if (!world.has<CTag>(e)) return;
    auto& tags = world.get<CTag>(e).tags;
    tags.erase(std::remove(tags.begin(), tags.end(), tag), tags.end());
}

class DestructibleMechanic final : public BuiltinMechanicBase<DestructibleMechanic> {
public:
    static constexpr const char* kId = "builtin.destructible";
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json&) {}

    void on_attach(World& world, std::uint32_t self, const nlohmann::json& p,
                   MechanicContext&) override {
        effect_ = p.value("effect", std::string("shatter"));
        respawn_s_ = std::clamp(p.value("respawn_after_s", 0.0f), 0.0f, 3600.0f);
        if (!world.has<CHealth>(self)) {
            CHealth h{};
            h.max = h.current = std::max(1.0f, p.value("max_health", 50.0f));
            h.invuln_after_hit_s = 0.0f;
            world.registry().emplace<CHealth>(static_cast<entt::entity>(self), h);
        }
        const std::string* id = world.id_of(static_cast<entt::entity>(self));
        name_ = id != nullptr ? *id : std::string("destructible");
        destroyed_ = false;
    }

    void tick(World& world, std::uint32_t self, float dt, const InputSnapshot&,
              MechanicContext& ctx) {
        if (!world.valid(self) || !world.has<CHealth>(self)) return;
        auto& hp = world.get<CHealth>(self);
        if (!destroyed_) {
            if (hp.alive && hp.current > 0.0f) return;
            destroy(world, self, ctx);
            return;
        }
        // Keep the engine from deleting it: it is broken, not gone.
        hp.death_timer_s = -1.0f;
        if (respawn_s_ <= 0.0f) return;
        timer_ -= dt;
        if (timer_ <= 0.0f) restore(world, self, ctx);
    }

private:
    void destroy(World& world, std::uint32_t self, MechanicContext& ctx) {
        destroyed_ = true;
        timer_ = respawn_s_;
        auto& hp = world.get<CHealth>(self);
        hp.alive = false;
        hp.current = 0.0f;
        hp.death_timer_s = -1.0f;
        Vec3 centre = world.get<CTransform>(self).t.position;
        if (ctx.physics != nullptr) ctx.physics->body_center(self, &centre);
        if (ctx.vfx != nullptr) {
            if (effect_ == "explode") {
                ctx.vfx->explosion(centre, 1.2f, Vec4(1.0f, 0.55f, 0.2f, 1.0f));
            } else if (effect_ != "vanish") {
                ctx.vfx->impact(centre, Vec3(0.0f, 1.0f, 0.0f), ImpactStyle::Dust,
                                Vec4(0.8f, 0.75f, 0.65f, 1.0f));
                ctx.vfx->impact(centre + Vec3(0.0f, 0.3f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                ImpactStyle::Sparks, Vec4(1.0f, 0.8f, 0.5f, 1.0f));
            }
        }
        // Hidden (the renderer skips "template") and out of the simulation,
        // so no invisible target keeps stopping shots.
        add_tag(world, self, "template");
        add_tag(world, self, "destroyed");
        if (ctx.physics != nullptr) ctx.physics->set_collision_enabled(self, false);
        if (ctx.interactive != nullptr && ctx.interactive->exists(name_.c_str())) {
            ctx.interactive->set_state(name_.c_str(), "destroyed", InteractiveValue::of(true));
        }
        if (ctx.signals != nullptr) {
            ctx.signals->emit((name_ + ".destroyed").c_str());
            ctx.signals->emit("destructible.destroyed");
        }
        if (ctx.vars != nullptr) {
            ctx.vars->set_number("destructibles.destroyed",
                                 ctx.vars->number("destructibles.destroyed") + 1.0f);
        }
        PTV_INFO("destructible: '{}' destroyed ({})", name_, effect_);
    }

    void restore(World& world, std::uint32_t self, MechanicContext& ctx) {
        destroyed_ = false;
        remove_tag(world, self, "template");
        remove_tag(world, self, "destroyed");
        if (ctx.physics != nullptr) ctx.physics->set_collision_enabled(self, true);
        auto& hp = world.get<CHealth>(self);
        hp.current = hp.max;
        hp.alive = true;
        hp.death_timer_s = -1.0f;
        hp.invuln_s = 0.0f;
        if (ctx.interactive != nullptr && ctx.interactive->exists(name_.c_str())) {
            ctx.interactive->set_state(name_.c_str(), "destroyed", InteractiveValue::of(false));
        }
        if (ctx.signals != nullptr) ctx.signals->emit((name_ + ".restored").c_str());
        PTV_INFO("destructible: '{}' restored", name_);
    }

    std::string effect_{"shatter"};
    std::string name_;
    float respawn_s_{0.0f};
    float timer_{0.0f};
    bool destroyed_{false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_destructible = 1;

PTV_REGISTER_MECHANIC("builtin.destructible", DestructibleMechanic)

}  // namespace ptv
