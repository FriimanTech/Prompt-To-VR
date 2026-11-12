// builtin.press_button -- a button, switch or plunger the player presses.
//
// The shooting range's red challenge button never worked: its generated code
// waited for a contact between the button and a hand, and the player's hands
// had no colliders. A press is not a physics contact anyway. This follows
// Meta's poke model (Interaction SDK): the INDEX FINGERTIP crosses the cap's
// surface along its normal between two frames, inside the cap's footprint --
// which handles fast jabs and refuses a press from behind or beside the cap.
// For controller players the trigger or grip also presses while the hand is
// at the button and empty.
//
// On press: the cap dips by ``travel_m`` and springs back (ctx.fx), a short
// haptic tick plays on that hand, and the named signal is raised
// (ctx.signals) -- a challenge, a door, a light wires itself to the signal
// rather than to this entity. A light hover highlight shows the button is
// pressable when a fingertip comes near.
//
// Params: signal (default "<entity id>.pressed"), press_with
// (poke | grip | trigger | any, default any), travel_m (0.007), cooldown_s
// (0.35), press_socket ("press_surface"), radius_m (0 = from the collider),
// reach_m (0.12, grip/trigger), haptic (0.5), require_gameplay (false).

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/GameFlow.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
/** Meta's minimum poke target is 22 mm. */
constexpr float kMinRadius = 0.011f;
constexpr float kPressDown = 0.7f;
/** Index tip relative to the grip pose, the same point the engine uses for
 *  a hand with no posable fingers. */
const Vec3 kPokeOffset(0.0f, -0.01f, -0.095f);
/** A drawn fingertip farther than this from the controller's poke point is
 *  trailing it. */
constexpr float kTipLagM = 0.08f;

class PressButtonMechanic final : public BuiltinMechanicBase<PressButtonMechanic> {
public:
    static constexpr const char* kId = "builtin.press_button";
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void configure(const nlohmann::json&) {}

    void on_attach(World& world, std::uint32_t self, const nlohmann::json& p,
                   MechanicContext&) override {
        signal_ = p.value("signal", std::string());
        if (signal_.empty()) {
            const std::string* id = world.id_of(static_cast<entt::entity>(self));
            signal_ = (id != nullptr ? *id : std::string("button")) + ".pressed";
        }
        const std::string with = p.value("press_with", std::string("any"));
        poke_ = with == "poke" || with == "any";
        grip_ = with == "grip" || with == "any";
        trigger_ = with == "trigger" || with == "any";
        travel_ = std::clamp(p.value("travel_m", 0.007f), 0.002f, 0.05f);
        cooldown_s_ = std::clamp(p.value("cooldown_s", 0.35f), 0.05f, 5.0f);
        socket_ = p.value("press_socket", std::string("press_surface"));
        radius_ = std::max(0.0f, p.value("radius_m", 0.0f));
        reach_ = std::clamp(p.value("reach_m", 0.12f), 0.03f, 0.5f);
        haptic_ = std::clamp(p.value("haptic", 0.5f), 0.0f, 1.0f);
        require_gameplay_ = p.value("require_gameplay", false);
    }

    void tick(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (!world.valid(self) || !world.has<CTransform>(self)) return;
        cooldown_ = std::max(0.0f, cooldown_ - dt);
        Vec3 cap(0.0f), n(0.0f, 1.0f, 0.0f);
        float radius = kMinRadius;
        cap_frame(world, self, ctx, cap, n, radius);

        float hover = 0.0f;
        int pressed_by = -1;
        const char* how = "";
        for (int h = 0; h < 2; ++h) {
            const auto& hs = input.hands[static_cast<std::size_t>(h)];
            if (!hs.hand_valid) {
                armed_[static_cast<std::size_t>(h)] = false;
                continue;
            }
            // Poke: the index fingertip crosses the cap plane, inward, inside
            // the footprint.
            Vec3 tip(0.0f);
            bool have_tip = ctx.hands != nullptr && ctx.hands->fingertip_world(h, "index", &tip);
            // The controller's own poke point decides when the drawn finger
            // is not at it: a full-body avatar's arm can trail the
            // controller, and a button must still press where the player's
            // real finger is.
            const Vec3 poke_point = hs.hand_position + hs.hand_rotation * kPokeOffset;
            if (!have_tip || glm::length(tip - poke_point) > kTipLagM) {
                tip = poke_point;
                have_tip = true;
            }
            if (poke_ && have_tip) {
                const Vec3 rel = tip - cap;
                const float depth = glm::dot(rel, n);
                const float lateral = glm::length(rel - n * depth);
                if (lateral <= radius * 1.15f && depth < 0.06f) {
                    hover = std::max(hover, 1.0f - std::clamp(depth / 0.06f, 0.0f, 1.0f));
                }
                // Armed while the tip is OUTSIDE the cap above its footprint;
                // a press is the armed tip pushed in by half the travel. It
                // re-arms only after leaving, so resting on the cap presses once
                // and a hand arriving from below or the side never presses.
                bool& armed = armed_[static_cast<std::size_t>(h)];
                if (depth > 0.002f && lateral <= radius * 1.15f) {
                    armed = true;
                } else if (lateral > radius * 1.15f || depth < -0.08f) {
                    armed = false;
                }
                if (armed && depth <= -0.5f * travel_ && lateral <= radius && pressed_by < 0) {
                    pressed_by = h;
                    how = "poke";
                    armed = false;
                }
            }
            // Controllers: trigger / grip while an EMPTY hand is at the button.
            const bool empty = ctx.hands == nullptr || ctx.hands->held_entity(h) == kNone;
            const bool near = glm::length((have_tip ? tip : hs.hand_position) - cap) <= reach_ ||
                              glm::length(hs.hand_position - cap) <= reach_;
            if (empty && near && pressed_by < 0) {
                if (trigger_ && hs.prev_trigger < kPressDown && hs.trigger >= kPressDown) {
                    pressed_by = h;
                    how = "trigger";
                } else if (grip_ && hs.prev_grip < kPressDown && hs.grip >= kPressDown) {
                    pressed_by = h;
                    how = "grip";
                }
            }
            if (empty && near) hover = std::max(hover, 0.5f);
        }

        auto& hl = world.registry().get_or_emplace<CHighlight>(static_cast<entt::entity>(self));
        hl.intensity = 0.35f * hover;
        hl.color = Vec4(1.0f, 0.4f, 0.3f, 1.0f);

        if (pressed_by < 0 || cooldown_ > 0.0f) return;
        if (require_gameplay_ && ctx.game != nullptr && !ctx.game->gameplay_active()) return;
        cooldown_ = cooldown_s_;
        ++presses_;
        if (ctx.signals != nullptr) ctx.signals->emit(signal_.c_str());
        if (ctx.vars != nullptr) {
            ctx.vars->set_number((signal_ + ".count").c_str(), static_cast<float>(presses_));
        }
        if (ctx.fx != nullptr) {
            // Dip along -normal in the entity's own frame.
            const Quat inv = glm::inverse(world.get<CTransform>(self).t.rotation);
            TransformImpulse kick{};
            kick.local_offset = inv * (-n * travel_);
            kick.recover_tau_s = 0.09f;
            ctx.fx->impulse(self, kick);
        }
        if (ctx.haptics != nullptr && haptic_ > 0.0f) {
            ctx.haptics->pulse_at(pressed_by, 0.03f, 220.0f, haptic_);
        }
        const std::string* id = world.id_of(static_cast<entt::entity>(self));
        PTV_INFO("press_button: '{}' pressed by {} hand ({}) -> signal '{}' #{}",
                 id != nullptr ? *id : std::string("?"), pressed_by == 0 ? "left" : "right",
                 how, signal_, presses_);
    }

private:
    /** The cap: the press socket when the asset has one (+Z out of the cap),
     *  else the top of the collider, facing up. */
    void cap_frame(World& world, std::uint32_t self, MechanicContext& ctx, Vec3& cap, Vec3& n,
                   float& radius) const {
        const auto& t = world.get<CTransform>(self).t;
        if (ctx.sockets != nullptr && ctx.sockets->has_socket(self, socket_.c_str())) {
            const Mat4 m = ctx.sockets->socket_world(self, socket_.c_str());
            cap = Vec3(m[3]);
            n = glm::normalize(Vec3(m[2]));
            radius = radius_ > 0.0f ? radius_ : 0.035f;
        } else if (world.has<CCollisionGeom>(self)) {
            const auto& g = world.get<CCollisionGeom>(self);
            const Vec3 top_local(0.5f * (g.aabb_min.x + g.aabb_max.x), g.aabb_max.y,
                                 0.5f * (g.aabb_min.z + g.aabb_max.z));
            cap = t.position + t.rotation * (top_local * t.scale);
            n = glm::normalize(t.rotation * Vec3(0.0f, 1.0f, 0.0f));
            const float fx = (g.aabb_max.x - g.aabb_min.x) * t.scale;
            const float fz = (g.aabb_max.z - g.aabb_min.z) * t.scale;
            radius = radius_ > 0.0f ? radius_ : std::clamp(0.4f * std::min(fx, fz), 0.02f, 0.15f);
        } else {
            cap = t.position;
            n = Vec3(0.0f, 1.0f, 0.0f);
            radius = radius_ > 0.0f ? radius_ : 0.04f;
        }
        radius = std::max(radius, kMinRadius);
    }

    std::string signal_;
    std::string socket_;
    bool poke_{true};
    bool grip_{true};
    bool trigger_{true};
    bool require_gameplay_{false};
    float travel_{0.007f};
    float cooldown_s_{0.35f};
    float cooldown_{0.0f};
    float radius_{0.0f};
    float reach_{0.12f};
    float haptic_{0.5f};
    int presses_{0};
    std::array<bool, 2> armed_{false, false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_press_button = 1;

PTV_REGISTER_MECHANIC("builtin.press_button", PressButtonMechanic)

}  // namespace ptv
