// builtin.equip -- wear things, and take them off again.
//
// With no parameters at all an equippable object goes anywhere on the body,
// which is the freedom-first default the design asked for. Everything past
// that is opt-in configuration.
//
// The two placement modes are the ones real prompts need:
//
//   * a SLOT WHITELIST, for a glove that belongs on a hand and nowhere else;
//   * a COVERAGE SPAN, for an Iron Man gauntlet that has to sit from the
//     fingertips to the elbow. A span is measured on the rig at runtime --
//     the distance from a fingertip to an elbow is a property of that rig in
//     its current pose, not a constant anyone can hard-code -- and the
//     object is fitted to it using its own derived mount sockets.
//
// Equip and unequip are INDEPENDENT triggers, which is what makes the two
// awkward prompt shapes fall out as pure configuration rather than new code:
// a suit that can never be removed is ``unequip_on: never``, and an
// escape-room game whose items can only ever come off is
// ``equip_on: never`` plus scene-start equipping.
//
// The default removal gesture is grab-to-pull-off with the OTHER hand. That
// is not a nicety: a glove on the right hand cannot be grabbed by the right
// hand, so a same-hand rule would ship a glove nobody can remove.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "../BuiltinMechanicBase.h"
#include "ptv/api/BodySlots.h"
#include "ptv/core/Logger.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv {
namespace {

enum class EquipOn : int {
    GrabRelease = 0,
    Button = 1,
    Contact = 2,
    Api = 3,
    SceneStart = 4,
    Never = 5,
};

enum class UnequipOn : int {
    GrabPull = 0,
    Button = 1,
    Api = 2,
    Never = 3,
};

/** What happens to an item displaced from an exclusive slot. */
enum class OnReplace : int { ToHand = 0, Drop = 1, Despawn = 2, Inventory = 3 };

EquipOn equip_on_from(const std::string& s) {
    if (s == "button") return EquipOn::Button;
    if (s == "contact") return EquipOn::Contact;
    if (s == "api") return EquipOn::Api;
    if (s == "scene_start") return EquipOn::SceneStart;
    if (s == "never") return EquipOn::Never;
    return EquipOn::GrabRelease;
}

UnequipOn unequip_on_from(const std::string& s) {
    if (s == "button") return UnequipOn::Button;
    if (s == "api") return UnequipOn::Api;
    if (s == "never") return UnequipOn::Never;
    return UnequipOn::GrabPull;
}

OnReplace on_replace_from(const std::string& s) {
    if (s == "drop") return OnReplace::Drop;
    if (s == "despawn") return OnReplace::Despawn;
    if (s == "inventory") return OnReplace::Inventory;
    return OnReplace::ToHand;
}

constexpr float kPullRadius = 0.14f;
constexpr float kGripPress = 0.6f;
constexpr float kGripRelease = 0.35f;

class EquipMechanic : public BuiltinMechanicBase<EquipMechanic> {
public:
    static constexpr const char* kId = "builtin.equip";

    // Input phase, matching grab: the decision has to be made before physics
    // integrates, or a released object falls a frame before it snaps on.
    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Entity; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext& ctx) override {
        self_ = entity;
        configure(p);

        // Binding this mechanic is what declares the object equippable, the
        // same way binding grab is what makes an object grabbable.
        auto& tag = world.registry().get_or_emplace<CTag>(
            static_cast<entt::entity>(entity));
        if (std::find(tag.tags.begin(), tag.tags.end(), "equippable") ==
            tag.tags.end()) {
            tag.tags.push_back("equippable");
        }

        // Seed the per-item flags so they exist before anything is worn --
        // set_removable / set_equippable must work on an item still lying on
        // the floor.
        auto& eq = world.registry().get_or_emplace<CEquipped>(
            static_cast<entt::entity>(entity));
        eq.passive_grab_protection = passive_grab_protection_;
        eq.removable = removable_;
        eq.re_equippable = re_equippable_;
        eq.group = group_;

        if (equip_on_ == EquipOn::SceneStart) pending_scene_equip_ = true;
        (void)ctx;
    }

    void configure(const nlohmann::json& p) {
        slots_.clear();
        if (p.contains("slots") && p["slots"].is_array()) {
            for (const auto& s : p["slots"]) {
                if (s.is_string()) slots_.push_back(s.get<std::string>());
            }
        }
        coverage_start_ = p.value("coverage_start", std::string());
        coverage_end_ = p.value("coverage_end", std::string());
        fit_to_span_ =
            p.value("fit_to_span", !coverage_start_.empty() &&
                                       !coverage_end_.empty());
        rest_slots_.clear();
        if (p.contains("rest_slots") && p["rest_slots"].is_array()) {
            for (const auto& s : p["rest_slots"]) {
                if (s.is_string()) rest_slots_.push_back(s.get<std::string>());
            }
        }
        auto_side_ = p.value("auto_side", true);
        snap_radius_ = std::clamp(p.value("snap_radius_m", 0.25f), 0.0f, 2.0f);

        equip_on_ = equip_on_from(
            p.value("equip_on", std::string("grab_release")));
        unequip_on_ = unequip_on_from(
            p.value("unequip_on", std::string("grab_pull")));
        unequip_requires_free_hand_ =
            p.value("unequip_requires_free_hand", true);
        unequip_with_other_hand_only_ =
            p.value("unequip_with_other_hand_only", true);

        passive_grab_protection_ = p.value("passive_grab_protection", true);
        removable_ = p.value("removable", true);
        re_equippable_ = p.value("re_equippable", true);
        exclusive_ = p.value("exclusive", true);
        on_replace_ = on_replace_from(
            p.value("on_replace", std::string("to_hand")));
        group_ = p.value("group", std::string());

        rotation_offset_deg_ = Vec3(0.0f);
        if (p.contains("rotation_offset_deg") &&
            p["rotation_offset_deg"].is_array() &&
            p["rotation_offset_deg"].size() == 3) {
            rotation_offset_deg_ =
                Vec3(p["rotation_offset_deg"][0].get<float>(),
                     p["rotation_offset_deg"][1].get<float>(),
                     p["rotation_offset_deg"][2].get<float>());
        }
        position_offset_ = Vec3(0.0f);
        if (p.contains("position_offset_m") &&
            p["position_offset_m"].is_array() &&
            p["position_offset_m"].size() == 3) {
            position_offset_ = Vec3(p["position_offset_m"][0].get<float>(),
                                    p["position_offset_m"][1].get<float>(),
                                    p["position_offset_m"][2].get<float>());
        }

        // Materialized from the ControllerBinding by the scene compiler.
        control_ = p.value("control", std::string("none"));
        hand_param_ = p.value("hand", std::string("either"));
    }

    void tick(World& world, std::uint32_t self, float, const InputSnapshot& in,
              MechanicContext& ctx) {
        if (ctx.equip == nullptr || ctx.hands == nullptr) return;
        const std::uint32_t wearer = ctx.hands->player_entity();

        if (pending_scene_equip_) {
            pending_scene_equip_ = false;
            const std::string slot = pick_slot(world, self, wearer, ctx);
            if (!slot.empty()) do_equip(world, self, wearer, slot, ctx);
        }

        for (int h = 0; h < 2; ++h) {
            const auto& hand = in.hands[h];
            const bool released = hand.prev_grip >= kGripPress &&
                                  hand.grip < kGripRelease;
            const bool pressed =
                hand.prev_grip < kGripPress && hand.grip >= kGripPress;

            if (equip_on_ == EquipOn::GrabRelease && released) {
                try_equip_from_hand(world, self, wearer, h, hand, ctx);
            }
            if (unequip_on_ == UnequipOn::GrabPull && pressed) {
                try_pull_off(world, self, wearer, h, hand, in, ctx);
            }
        }

        if (equip_on_ == EquipOn::Button && button_edge(in)) {
            const std::string slot = pick_slot(world, self, wearer, ctx);
            if (!slot.empty()) do_equip(world, self, wearer, slot, ctx);
        }
        if (unequip_on_ == UnequipOn::Button && button_edge(in)) {
            ctx.equip->unequip(self);
        }
        if (equip_on_ == EquipOn::Contact && ctx.contacts != nullptr &&
            ctx.contacts->began(self, wearer)) {
            const std::string slot = pick_slot(world, self, wearer, ctx);
            if (!slot.empty()) do_equip(world, self, wearer, slot, ctx);
        }
    }

    void on_detach(World&, std::uint32_t) override {}

private:
    bool button_edge(const InputSnapshot& in) const {
        // Rising edge only; holding a button must not re-fire every frame.
        const bool now = button_down(in);
        const bool was = button_was_down_;
        button_was_down_ = now;
        return now && !was;
    }

    bool button_down(const InputSnapshot& in) const {
        if (control_ == "a") return in.button_a;
        if (control_ == "b") return in.button_b;
        if (control_ == "x") return in.button_x;
        if (control_ == "y") return in.button_y;
        if (control_ == "trigger") {
            const int h = hand_param_ == "left" ? 0 : 1;
            return in.hands[h].trigger >= 0.6f;
        }
        return false;
    }

    /** Equip when the player lets go of us near a compatible slot. */
    void try_equip_from_hand(World& world, std::uint32_t self,
                             std::uint32_t wearer, int hand,
                             const InputSnapshot::HandState& hs,
                             MechanicContext& ctx) {
        if (hs.held_entity != self) return;
        const std::string slot =
            pick_slot_near(world, self, wearer, hs.hand_position, ctx, hand);
        if (slot.empty()) return;
        ctx.hands->set_held_entity(hand, InputSnapshot::kInvalidEntity);
        do_equip(world, self, wearer, slot, ctx);
    }

    /** Deliberate removal: reach to the worn item and squeeze. */
    void try_pull_off(World& world, std::uint32_t self, std::uint32_t wearer,
                      int hand, const InputSnapshot::HandState& hs,
                      const InputSnapshot& in, MechanicContext& ctx) {
        (void)world;
        if (!ctx.equip->is_removable(self)) return;
        const char* slot = ctx.equip->slot_of(self);
        if (slot == nullptr || *slot == '\0') return;

        // A glove worn on this hand cannot be grabbed by this hand, so the
        // rule is structural rather than a preference.
        if (unequip_with_other_hand_only_ && slot_is_on_hand(slot, hand)) {
            return;
        }
        if (unequip_requires_free_hand_ &&
            hs.held_entity != InputSnapshot::kInvalidEntity) {
            return;
        }
        if (!hs.hand_valid) return;

        if (!world_position_within(ctx, self, hs.hand_position, kPullRadius)) {
            return;
        }
        if (!ctx.equip->unequip(self)) return;

        // Straight into that hand, so it flows into throw exactly as a
        // normal grab would.
        ctx.hands->set_held_entity(hand, self);
        if (ctx.haptics != nullptr) ctx.haptics->pulse(hand, 0.04f, 0.4f);
        (void)in;
    }

    bool slot_is_on_hand(const std::string& slot, int hand) const {
        const std::string suffix = body_side_suffix(hand);
        return slot.size() > suffix.size() &&
               slot.compare(slot.size() - suffix.size(), suffix.size(),
                            suffix) == 0;
    }

    bool world_position_within(MechanicContext& ctx, std::uint32_t entity,
                               const Vec3& point, float radius) const {
        if (ctx.sockets == nullptr) return true;
        const Mat4 m = ctx.sockets->socket_world(entity, "grip");
        return glm::length(Vec3(m[3]) - point) <= radius;
    }

    /** Which slot should this object occupy? */
    std::string pick_slot(World& world, std::uint32_t self,
                          std::uint32_t wearer, MechanicContext& ctx) {
        return pick_slot_near(world, self, wearer, Vec3(0.0f), ctx, -1,
                              /*ignore_proximity=*/true);
    }

    std::string pick_slot_near(World& world, std::uint32_t self,
                               std::uint32_t wearer, const Vec3& near_point,
                               MechanicContext& ctx, int hand,
                               bool ignore_proximity = false) {
        const auto we = static_cast<entt::entity>(wearer);
        if (!world.registry().all_of<CEquipSlots>(we)) return {};
        const auto& all = world.registry().get<CEquipSlots>(we).slots;

        // A span declares its own destination: the parent-most end of the
        // coverage is the joint the object hangs off.
        if (!coverage_start_.empty() && !coverage_end_.empty()) {
            const std::string root =
                span_root_slot(self, wearer, ctx);
            if (!root.empty()) return root;
        }

        std::string best;
        float best_d = std::numeric_limits<float>::max();
        for (const auto& s : all) {
            if (!slots_.empty() &&
                std::find(slots_.begin(), slots_.end(), s.name) ==
                    slots_.end()) {
                continue;
            }
            if (auto_side_ && hand >= 0 && !side_matches(s.name, self, hand)) {
                continue;
            }
            if (s.exclusive && !exclusive_) {
                // The object says it may coexist; respect the SLOT's rule,
                // which is the stricter of the two.
            }
            if (ignore_proximity || s.snap_radius_m <= 0.0f) {
                if (best.empty()) best = s.name;
                continue;
            }
            const Mat4 anchor = slot_world(ctx, wearer, s);
            const float d = glm::length(Vec3(anchor[3]) - near_point);
            const float radius = std::max(s.snap_radius_m, snap_radius_);
            if (d <= radius && d < best_d) {
                best_d = d;
                best = s.name;
            }
        }
        return best;
    }

    /** Auto-side: a left-named object prefers left slots. */
    bool side_matches(const std::string& slot, std::uint32_t self,
                      int hand) const {
        (void)self;
        const bool slot_left = slot_is_on_hand(slot, 0);
        const bool slot_right = slot_is_on_hand(slot, 1);
        if (!slot_left && !slot_right) return true;  // unsided slot
        return hand == 0 ? slot_left : slot_right;
    }

    Mat4 slot_world(MechanicContext& ctx, std::uint32_t wearer,
                    const EquipSlot& s) const {
        if (ctx.skeleton != nullptr && !s.body_slot.empty()) {
            const int j =
                ctx.skeleton->joint_for_slot(wearer, s.body_slot.c_str());
            if (j >= 0) {
                return ctx.skeleton->joint_world(wearer, j) *
                       glm::translate(Mat4(1.0f), s.local_position);
            }
        }
        if (ctx.sockets != nullptr) {
            return ctx.sockets->socket_world(wearer, s.name.c_str());
        }
        return Mat4(1.0f);
    }

    /** Parent-most end of the declared coverage span.
     *
     *  A rigid attachment needs one parent, and for a fingertip-to-elbow
     *  gauntlet that parent has to be the elbow: everything else in the span
     *  hangs off it, so anchoring anywhere further down would leave the
     *  forearm section floating. */
    std::string span_root_slot(std::uint32_t self, std::uint32_t wearer,
                               MechanicContext& ctx) const {
        (void)self;
        if (ctx.skeleton == nullptr) return {};
        const int a = ctx.skeleton->joint_for_slot(wearer,
                                                   coverage_start_.c_str());
        const int b =
            ctx.skeleton->joint_for_slot(wearer, coverage_end_.c_str());
        if (a < 0 && b < 0) return {};
        if (a < 0) return coverage_end_;
        if (b < 0) return coverage_start_;

        // Walk up from a; if b is an ancestor, b is the root.
        for (int j = a; j >= 0; j = ctx.skeleton->joint_parent(wearer, j)) {
            if (j == b) return coverage_end_;
        }
        return coverage_start_;
    }

    void do_equip(World& world, std::uint32_t self, std::uint32_t wearer,
                  const std::string& slot, MechanicContext& ctx) {
        if (equip_on_ == EquipOn::Never) return;

        // Handle the displaced incumbent before the slot changes hands, so
        // the disposition policy sees the object that is actually leaving.
        const std::uint32_t incumbent =
            ctx.equip->equipped_at(wearer, slot.c_str());
        if (!ctx.equip->equip(self, wearer, slot.c_str())) return;

        if (incumbent != InputSnapshot::kInvalidEntity && incumbent != self) {
            dispose_of(incumbent, ctx);
        }

        // Fit and trim. The span fit is what makes a gauntlet match the arm
        // it is on rather than the arm the artist happened to model.
        Vec3 offset = position_offset_;
        Quat rot = glm::quat(glm::radians(rotation_offset_deg_));
        if (fit_to_span_) apply_span_fit(world, self, wearer, ctx);
        if (offset != Vec3(0.0f) || rotation_offset_deg_ != Vec3(0.0f)) {
            ctx.equip->set_offset(self, offset, rot);
        }
    }

    void dispose_of(std::uint32_t item, MechanicContext& ctx) {
        switch (on_replace_) {
            case OnReplace::Despawn:
                if (ctx.spawn != nullptr) ctx.spawn->despawn(item);
                break;
            case OnReplace::ToHand:
            case OnReplace::Drop:
            case OnReplace::Inventory:
            default:
                // Already detached by the service; leaving it in world space
                // covers drop, and to_hand/inventory are game policy the
                // owning generated mechanic applies.
                break;
        }
    }

    /** Scale the object so its own long axis matches the measured span. */
    void apply_span_fit(World& world, std::uint32_t self, std::uint32_t wearer,
                        MechanicContext& ctx) {
        if (ctx.skeleton == nullptr || ctx.sockets == nullptr) return;
        const int a =
            ctx.skeleton->joint_for_slot(wearer, coverage_start_.c_str());
        const int b =
            ctx.skeleton->joint_for_slot(wearer, coverage_end_.c_str());
        if (a < 0 || b < 0) return;

        const Vec3 pa(ctx.skeleton->joint_world(wearer, a)[3]);
        const Vec3 pb(ctx.skeleton->joint_world(wearer, b)[3]);
        const float span = glm::length(pb - pa);
        if (span < 1.0e-3f) return;

        // The object's own extent comes from the sockets asset_contract
        // derived, so nothing here has to inspect a mesh.
        const Vec3 base(ctx.sockets->socket_world(self, "mount_base")[3]);
        const Vec3 tip(ctx.sockets->socket_world(self, "mount_tip")[3]);
        const float own = glm::length(tip - base);
        if (own < 1.0e-3f) return;

        const auto e = static_cast<entt::entity>(self);
        if (!world.registry().all_of<CTransform>(e)) return;
        auto& t = world.registry().get<CTransform>(e).t;
        const float factor = std::clamp(span / own, 0.25f, 4.0f);
        t.scale *= factor;
    }

    std::uint32_t self_{InputSnapshot::kInvalidEntity};
    std::vector<std::string> slots_;
    std::string coverage_start_;
    std::string coverage_end_;
    bool fit_to_span_{false};
    std::vector<std::string> rest_slots_;
    bool auto_side_{true};
    float snap_radius_{0.25f};

    EquipOn equip_on_{EquipOn::GrabRelease};
    UnequipOn unequip_on_{UnequipOn::GrabPull};
    bool unequip_requires_free_hand_{true};
    bool unequip_with_other_hand_only_{true};

    bool passive_grab_protection_{true};
    bool removable_{true};
    bool re_equippable_{true};
    bool exclusive_{true};
    OnReplace on_replace_{OnReplace::ToHand};
    std::string group_;

    Vec3 rotation_offset_deg_{0.0f};
    Vec3 position_offset_{0.0f};
    std::string control_{"none"};
    std::string hand_param_{"either"};

    bool pending_scene_equip_{false};
    mutable bool button_was_down_{false};
};

}  // namespace

// Anchor so MSVC keeps this TU (and its registration) when linking
// ptv_runtime as a static library. Referenced by BuiltinMechanics.cpp.
extern "C" int ptv_force_link_builtin_equip = 1;

PTV_REGISTER_MECHANIC("builtin.equip", EquipMechanic)

}  // namespace ptv
