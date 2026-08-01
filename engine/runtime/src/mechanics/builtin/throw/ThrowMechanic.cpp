// builtin.throw -- release a grabbed object with the hand's momentum.
//
// Entity-scoped on purpose: throwability is a property of the OBJECT, not of
// the player. The starter room binds throw to three of its four cubes, and
// the fourth is grabbable but only droppable. Each binding carries its own
// ``impulse`` strength, so a scene can have a light ball and a heavy crate.
//
// Runs in the Input phase, right after grab publishes the release, so the
// impulse lands before the physics step rather than a frame later.

#include <algorithm>

#include "../BuiltinMechanicBase.h"
#include "../grab/GrabRelease.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// The measured hand velocity alone throws disappointingly softly: the wrist
// snap that carries a real throw happens faster than the tracking smoothing
// can follow. Scaling up compensates. This reproduces the engine's previous
// hard-coded 1.5 * 0.4 kg factor.
constexpr float kVelocityScale = 1.5f;
constexpr float kDefaultMassKg = 0.4f;

class ThrowMechanic : public BuiltinMechanicBase<ThrowMechanic> {
public:
    static constexpr const char* kId = "builtin.throw";

    MechanicPhase phase() const override { return MechanicPhase::Input; }

    void configure(const nlohmann::json& p) {
        // "impulse" is documented as the throw strength. It is a scale on the
        // hand's momentum, not an absolute impulse -- a flick and a shove
        // must not throw the same distance.
        impulse_scale_ = p.value("impulse", 4.0f) / 4.0f;
    }

    void tick(World&, std::uint32_t entity, float, const InputSnapshot&,
              MechanicContext& ctx) {
        if (ctx.physics == nullptr) return;
        const auto& ev = grab::last_release();
        if (ev.serial == seen_serial_) return;
        seen_serial_ = ev.serial;
        if (ev.entity != entity) return;  // someone else's object

        const Vec3 impulse =
            ev.hand_velocity * kVelocityScale * kDefaultMassKg * impulse_scale_;
        ctx.physics->apply_impulse(entity, impulse);
        PTV_INFO("Throw: entity {} impulse {:.2f}", entity,
                 glm::length(impulse));
    }

private:
    float impulse_scale_{1.0f};
    std::uint64_t seen_serial_{0};
};

}  // namespace

extern "C" int ptv_force_link_builtin_throw = 1;

PTV_REGISTER_MECHANIC("builtin.throw", ThrowMechanic)

}  // namespace ptv
