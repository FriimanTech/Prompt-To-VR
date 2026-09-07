// builtin.door / builtin.container / builtin.mover -- things that move
// between declared configurations.
//
// All three are one idea with different vocabularies, which is why they share
// a translation unit: something declared a hinge or a slide, the player acts
// on it, and its new configuration is written back through ctx.interactive so
// it survives leaving and returning.
//
//   * door      -- continuous angle, grabbed by the handle and dragged. The
//                  range comes from the world builder (0-90 for a standard
//                  door, more where the design allows), matching how Meta's
//                  own SDK models a door as a constrained rotate about a
//                  pivot with min/max limits.
//   * container -- a drawer, cabinet or chest. Same motion, but the state
//                  worth recording is open versus closed rather than an
//                  exact angle, and its CONTENTS are tracked separately from
//                  the lid.
//   * mover     -- named positions rather than a range: elevator floors, a
//                  drawbridge raised or lowered, a barrier arm, retractable
//                  stairs. Cycling through the list is the whole interface.
//
// None of them stores its own state. A mechanic that cached "am I open"
// would lose it the moment the environment reloaded, which is exactly the
// bug the registry exists to prevent.

#include "AffordanceCommon.h"

namespace ptv {
namespace {

using namespace ptv::affordance;

/** Shared motion driver. Both hinge and slide are expressed as a normalized
 *  0..1 "openness", so one smoothing path serves every subclass. */
class MotionAffordanceBase : public IMechanic {
public:
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext& ctx) override {
        self_ = entity;
        open_tau_ = std::clamp(p.value("motion_tau_s", 0.18f), 0.0f, 2.0f);
        // Some things are operated at a distance (an elevator call button, a
        // bridge winch) rather than by touching them.
        reach_ = std::clamp(p.value("reach_m", kReach), 0.02f, 3.0f);
        base_.ensure(world, entity);

        // Seed the visual pose from whatever the document declared, so a
        // world that starts with a door already open LOOKS that way on the
        // first frame instead of snapping open once the player arrives.
        id_ = declared_id(ctx, entity);
        openness_ = target_openness(world, ctx);
        visual_ = openness_;
        (void)ctx;
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t self, float dt,
                   const InputSnapshot& input, MechanicContext& ctx) override {
        base_.ensure(world, self);
        if (!base_.captured || ctx.interactive == nullptr) return;
        if (id_.empty()) id_ = declared_id(ctx, self);
        if (id_.empty()) return;

        const CInteractive* ci = record(world, self);
        if (ci == nullptr) return;

        // Player input, only when the policy and the lock both allow it. A
        // locked gate still ANIMATES to whatever state game logic sets; what
        // is refused is the player changing it by hand.
        const bool allowed = actionable(ctx, id_);
        const Vec3 handle = handle_world(world, self, *ci, ctx);
        // Advertise reachability from the same test that gates the action, so
        // the cue can never promise something this mechanic will refuse.
        publish_cue(world, self, input, handle, allowed, dt, reach_ * 2.0f);
        if (allowed) {
            for (int h = 0; h < 2; ++h) {
                if (grip_started_near(input, h, handle, reach_)) {
                    on_player_grip(world, self, *ci, ctx);
                    break;
                }
            }
        }

        // Follow whatever the registry now says, whoever set it -- the
        // player, a generated mechanic, a timer, or a restored save.
        openness_ = target_openness(world, ctx);
        visual_ = approach(visual_, openness_, open_tau_, dt);
        apply_motion(world, self, *ci, ctx, dt);
    }

protected:
    /** 0 = closed / first position, 1 = fully open / last position. */
    virtual float target_openness(World& world, MechanicContext& ctx) = 0;
    /** React to a deliberate grip on the object. */
    virtual void on_player_grip(World& world, std::uint32_t self,
                                const CInteractive& ci,
                                MechanicContext& ctx) = 0;

    /** Where the player has to reach to operate this. The declared hinge is
     *  the best available proxy for a handle: it is the one point on the
     *  object the author placed deliberately. */
    Vec3 handle_world(World& world, std::uint32_t self, const CInteractive& ci,
                      MechanicContext& ctx) const {
        // A real grip socket wins when the asset has one.
        if (ctx.sockets != nullptr && ctx.sockets->has_socket(self, "grip")) {
            return Vec3(ctx.sockets->socket_world(self, "grip")[3]);
        }
        const auto e = static_cast<entt::entity>(self);
        if (!world.registry().all_of<CTransform>(e)) return Vec3(0.0f);
        const auto& t = world.registry().get<CTransform>(e).t;
        // Opposite the hinge is where a handle actually is on a door.
        return t.position + t.rotation * (-ci.hinge_point);
    }

    void apply_motion(World& world, std::uint32_t self, const CInteractive& ci,
                      MechanicContext& ctx, float dt) {
        if (ci.slide_distance_m > 1.0e-4f) {
            apply_slide(world, self, base_.value, ci.slide_axis,
                        visual_ * ci.slide_distance_m, ctx, dt);
            return;
        }
        const float angle = glm::radians(
            ci.min_angle_deg + visual_ * (ci.max_angle_deg - ci.min_angle_deg));
        apply_pivot_rotation(world, self, base_.value, ci.hinge_point,
                             ci.hinge_axis, angle, ctx, dt);
    }

    std::uint32_t self_{0xFFFFFFFFu};
    std::string id_;
    BasePose base_{};
    float openness_{0.0f};
    float visual_{0.0f};
    float open_tau_{0.18f};
    float reach_{kReach};
};

/** A hinged or sliding door, dragged open by hand. */
class DoorMechanic final : public MotionAffordanceBase {
public:
    std::string id() const override { return "builtin.door"; }

protected:
    float target_openness(World& world, MechanicContext& ctx) override {
        (void)world;
        // "angle" is authoritative when the world declared one, because a
        // door is genuinely continuous; "open" is the coarse fallback.
        const auto angle = ctx.interactive->get_state(id_.c_str(), "angle");
        if (angle.type == InteractiveValue::Type::Number) {
            const CInteractive* ci = record(world, self_);
            if (ci == nullptr) return 0.0f;
            const float span = ci->max_angle_deg - ci->min_angle_deg;
            if (std::fabs(span) < 1.0e-4f) return 0.0f;
            return std::clamp((angle.number - ci->min_angle_deg) / span, 0.0f,
                              1.0f);
        }
        return ctx.interactive->get_state(id_.c_str(), "open").b ? 1.0f : 0.0f;
    }

    void on_player_grip(World& world, std::uint32_t self,
                        const CInteractive& ci, MechanicContext& ctx) override {
        (void)world;
        (void)self;
        // A grip toggles between the two ends of the declared range. Full
        // one-to-one hand tracking of the swing is a refinement a
        // code-edited copy of this built-in can add; the base behaviour has
        // to be reliable first.
        const bool open_now = openness_ > 0.5f;
        const float want = open_now ? ci.min_angle_deg : ci.max_angle_deg;
        ctx.interactive->set_state(id_.c_str(), "open",
                                   InteractiveValue::of(!open_now));
        ctx.interactive->set_state(id_.c_str(), "angle",
                                   InteractiveValue::of(want));
    }
};

/** A drawer, cabinet or chest. */
class ContainerMechanic final : public MotionAffordanceBase {
public:
    std::string id() const override { return "builtin.container"; }

protected:
    float target_openness(World& world, MechanicContext& ctx) override {
        (void)world;
        const auto open = ctx.interactive->get_state(id_.c_str(), "open");
        if (open.type == InteractiveValue::Type::Number) {
            // "partially_open" is a real authored state, so a number is
            // honoured directly rather than rounded to a boolean.
            return std::clamp(open.number, 0.0f, 1.0f);
        }
        return open.b ? 1.0f : 0.0f;
    }

    void on_player_grip(World& world, std::uint32_t self,
                        const CInteractive& ci, MechanicContext& ctx) override {
        (void)world;
        (void)self;
        (void)ci;
        ctx.interactive->set_state(id_.c_str(), "open",
                                   InteractiveValue::of(openness_ <= 0.5f));
    }
};

/** Named positions: elevator floors, raised / lowered, stored / deployed. */
class MoverMechanic final : public MotionAffordanceBase {
public:
    std::string id() const override { return "builtin.mover"; }

protected:
    float target_openness(World& world, MechanicContext& ctx) override {
        const CInteractive* ci = record(world, self_);
        if (ci == nullptr || ci->positions.size() < 2) {
            return ctx.interactive->get_state(id_.c_str(), "open").b ? 1.0f
                                                                     : 0.0f;
        }
        const auto pos = ctx.interactive->get_state(id_.c_str(), "position");
        const auto it = std::find(ci->positions.begin(), ci->positions.end(),
                                  pos.name);
        if (it == ci->positions.end()) return 0.0f;
        const auto index =
            static_cast<float>(std::distance(ci->positions.begin(), it));
        return index / static_cast<float>(ci->positions.size() - 1);
    }

    void on_player_grip(World& world, std::uint32_t self,
                        const CInteractive& ci, MechanicContext& ctx) override {
        (void)world;
        (void)self;
        if (ci.positions.empty()) {
            ctx.interactive->set_state(id_.c_str(), "open",
                                       InteractiveValue::of(openness_ <= 0.5f));
            return;
        }
        // Advance to the next declared position, wrapping. An elevator that
        // should only be called to a specific floor is generated code
        // calling set_state directly; cycling is the sensible default when
        // the only interface is a hand.
        const auto current =
            ctx.interactive->get_state(id_.c_str(), "position");
        auto it = std::find(ci.positions.begin(), ci.positions.end(),
                            current.name);
        std::size_t next = 0;
        if (it != ci.positions.end()) {
            next = (static_cast<std::size_t>(
                        std::distance(ci.positions.begin(), it)) +
                    1) %
                   ci.positions.size();
        }
        ctx.interactive->set_state(id_.c_str(), "position",
                                   InteractiveValue::named(ci.positions[next]));
    }
};

}  // namespace

// Anchor so MSVC keeps this TU (and its registrations) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_motion_affordances = 1;

PTV_REGISTER_MECHANIC("builtin.door", DoorMechanic)
PTV_REGISTER_MECHANIC("builtin.container", ContainerMechanic)
PTV_REGISTER_MECHANIC("builtin.mover", MoverMechanic)

}  // namespace ptv
