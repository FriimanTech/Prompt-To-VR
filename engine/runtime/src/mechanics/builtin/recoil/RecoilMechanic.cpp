// builtin.recoil -- kick an object backward when its control is pressed.
//
// This is the cheapest "the weapon fired" cue there is, and it needs no
// sockets, no part separation and no skinning: the whole object shoves back
// along its own axis and springs home over ~100 ms.
//
// Entity-scoped: recoil is a property of the OBJECT. A pistol and a rocket
// launcher in the same scene each carry their own kick strength in params.
//
// Simulate phase, deliberately. The kick is a VISUAL offset written through
// ``ctx.fx``, and the engine ticks those offsets right after Simulate. Running
// in the Input phase instead would mean a kick requested on frame N is not
// drawn until frame N+1.
//
// Note what this mechanic does NOT do: it never writes ``CTransform``. For any
// entity with a physics body that transform is an output of the physics step
// (``PhysicsWorld::sync_to_world`` rewrites it every frame), and builtin.grab
// drives held objects by commanding physics rather than by setting the
// transform. Writing a kick there would either be erased on the next sync or
// silently desync the drawn pose from the one raycasts and collision use.

#include <algorithm>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// Defaults sized for a one-handed sidearm: a few centimetres of shove and a
// couple of degrees of muzzle rise, snapping back fast enough to be ready for
// the next shot.
constexpr float kDefaultKickBackM = 0.035f;
constexpr float kDefaultKickUpDeg = 3.0f;
constexpr float kDefaultRecoverTau = 0.07f;

// Analog controls fire on the rising edge through this threshold. Matches the
// grip threshold builtin.grab uses, so "pressed" means the same thing across
// the engine.
constexpr float kAnalogPress = 0.7f;

/** Which physical control drives the kick. Mirrors ``ControllerControl`` in
 *  packages/schemas/ptv_schemas/mechanic_plan.py; the scene compiler
 *  materializes the chosen value into this binding's ``control`` param. */
enum class Control { Trigger, Grip, A, B, X, Y };

Control parse_control(const std::string& s) {
    if (s == "grip") return Control::Grip;
    if (s == "a") return Control::A;
    if (s == "b") return Control::B;
    if (s == "x") return Control::X;
    if (s == "y") return Control::Y;
    return Control::Trigger;
}

/** True on the frame ``control`` transitions from released to pressed. */
bool control_rising_edge(Control control, const InputSnapshot& input, int hand) {
    const auto& h = input.hands[hand];
    switch (control) {
        case Control::Trigger:
            return h.prev_trigger < kAnalogPress && h.trigger >= kAnalogPress;
        case Control::Grip:
            return h.prev_grip < kAnalogPress && h.grip >= kAnalogPress;
        // Face buttons are global rather than per-hand: A/B live on the right
        // controller and X/Y on the left, so the hand is implied by the button.
        case Control::A:
            return !input.prev_button_a && input.button_a;
        case Control::B:
            return !input.prev_button_b && input.button_b;
        case Control::X:
            return !input.prev_button_x && input.button_x;
        case Control::Y:
            return !input.prev_button_y && input.button_y;
    }
    return false;
}

class RecoilMechanic : public BuiltinMechanicBase<RecoilMechanic> {
public:
    static constexpr const char* kId = "builtin.recoil";

    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void configure(const nlohmann::json& p) {
        control_ = parse_control(p.value("control", std::string("trigger")));

        // "either" (or an absent hand) means "whichever hand is holding this",
        // which is what a weapon almost always wants. An explicit left/right
        // pins the kick to one controller for a fixed-mount turret or a prop
        // that is never picked up.
        const std::string hand = p.value("hand", std::string("either"));
        if (hand == "left") {
            hand_ = InputSnapshot::Left;
        } else if (hand == "right") {
            hand_ = InputSnapshot::Right;
        } else {
            hand_ = -1;  // follow the holder
        }

        kick_back_m_ = std::max(0.0f, p.value("kick_back_m", kDefaultKickBackM));
        kick_up_deg_ = p.value("kick_up_deg", kDefaultKickUpDeg);
        recover_tau_s_ =
            std::max(0.01f, p.value("recover_tau_s", kDefaultRecoverTau));
        // Default on: a dropped weapon lying on the floor should not twitch
        // every time the player pulls the trigger on empty air.
        require_held_ = p.value("require_held", true);
    }

    void tick(World&, std::uint32_t entity, float, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (ctx.fx == nullptr) return;

        const int hand = resolve_hand(input, entity);
        if (hand < 0) return;
        if (require_held_ && input.hands[hand].held_entity != entity) return;
        if (!control_rising_edge(control_, input, hand)) return;

        TransformImpulse kick{};
        // -Z is "forward" for a weapon laid out along its barrel, so the shove
        // is +Z: backward, into the player's palm.
        kick.local_offset = Vec3{0.0f, 0.0f, kick_back_m_};
        // Muzzle rise: pitch the nose up about the object's own X axis.
        kick.local_euler_deg = Vec3{-kick_up_deg_, 0.0f, 0.0f};
        kick.recover_tau_s = recover_tau_s_;
        ctx.fx->impulse(entity, kick);
    }

private:
    /** The hand whose control we watch. When ``hand_`` is -1 this follows
     *  whichever hand currently holds the entity. */
    int resolve_hand(const InputSnapshot& input, std::uint32_t entity) const {
        if (hand_ >= 0) return hand_;
        for (int h = 0; h < 2; ++h) {
            if (input.hands[h].held_entity == entity) return h;
        }
        // Not held by anyone. With require_held off, a fixed prop still needs
        // some hand to read the control from; the right one is the default
        // primary hand throughout the pipeline.
        return require_held_ ? -1 : static_cast<int>(InputSnapshot::Right);
    }

    Control control_{Control::Trigger};
    int hand_{-1};
    float kick_back_m_{kDefaultKickBackM};
    float kick_up_deg_{kDefaultKickUpDeg};
    float recover_tau_s_{kDefaultRecoverTau};
    bool require_held_{true};
};

}  // namespace

extern "C" int ptv_force_link_builtin_recoil = 1;

PTV_REGISTER_MECHANIC("builtin.recoil", RecoilMechanic)

}  // namespace ptv
