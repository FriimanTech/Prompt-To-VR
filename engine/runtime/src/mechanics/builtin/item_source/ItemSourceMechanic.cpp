// builtin.item_source -- reach to a place on your body and draw a copy of an
// item into your hand: arrows from a quiver over the shoulder, magazines from
// the hip, grenades from the chest.
//
// Bound to a mesh-less SOURCE entity whose ``template`` param names the
// item's hidden template (tagged "template"; the scene compiler emits both --
// a template itself is never ticked). Each frame, for each EMPTY hand that
// just squeezed within ``radius_m`` of the source, a clone of the template is
// spawned at that hand and handed to builtin.grab through
// ``IHands::request_hold``, so it is held exactly as if the player had picked
// it up. ``count`` 0 is unlimited; ``max_live`` caps copies in the world (the
// oldest is recycled).
//
// The place is a body slot relative to the HEAD (the one pose every player
// has, Hands Only or full avatar): back, shoulder_r / shoulder_l, hip_r /
// hip_l, waist, chest. ``world_point`` places the source in the scene instead
// (an ammo crate).

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kGripPress = 0.7f;

/** Body-slot offsets in the head's yaw frame (+X right, +Y up, -Z forward). */
Vec3 slot_offset(const std::string& slot) {
    if (slot == "shoulder_r") return Vec3(0.17f, -0.22f, 0.12f);
    if (slot == "shoulder_l") return Vec3(-0.17f, -0.22f, 0.12f);
    if (slot == "hip_r") return Vec3(0.2f, -0.78f, 0.0f);
    if (slot == "hip_l") return Vec3(-0.2f, -0.78f, 0.0f);
    if (slot == "waist") return Vec3(0.0f, -0.72f, -0.12f);
    if (slot == "chest") return Vec3(0.0f, -0.38f, -0.15f);
    return Vec3(0.0f, -0.15f, 0.2f);  // back: behind the head, over the shoulders
}

class ItemSourceMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.item_source"; }
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void on_attach(World& world, std::uint32_t, const nlohmann::json& p,
                   MechanicContext&) override {
        const std::string tmpl = p.value("template", std::string());
        if (!tmpl.empty()) {
            const auto e = world.find(tmpl);
            if (e != entt::null) template_ = static_cast<std::uint32_t>(e);
        }
        if (template_ == kNone) {
            PTV_WARN("builtin.item_source: template '{}' not found; nothing to draw", tmpl);
        }
        slot_ = p.value("body_slot", std::string("back"));
        radius_ = std::clamp(p.value("radius_m", 0.25f), 0.05f, 2.0f);
        count_ = std::max(0, p.value("count", 0));
        max_live_ = std::clamp(p.value("max_live", 12), 1, 64);
        if (p.contains("world_point") && p["world_point"].is_array() &&
            p["world_point"].size() == 3) {
            const auto& a = p["world_point"];
            world_point_ = Vec3(a[0].get<float>(), a[1].get<float>(), a[2].get<float>());
            has_world_point_ = true;
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t, float, const InputSnapshot& input,
                   MechanicContext& ctx) override {
        if (ctx.spawn == nullptr || ctx.hands == nullptr) return;
        if (template_ == kNone || !world.valid(template_)) return;
        if (count_ > 0 && drawn_ >= count_) return;
        if (ctx.game != nullptr && !ctx.game->gameplay_active()) return;
        const Vec3 source = source_point(ctx);
        // Forget copies that no longer exist.
        while (!live_.empty() && !world.valid(live_.front())) live_.pop_front();
        for (int h = 0; h < 2; ++h) {
            const auto& hand = input.hands[h];
            if (!hand.hand_valid || hand.held_entity != kNone) continue;
            if (!(hand.prev_grip < kGripPress && hand.grip >= kGripPress)) continue;
            if (glm::length(hand.hand_position - source) > radius_) continue;
            if (static_cast<int>(live_.size()) >= max_live_) {
                ctx.spawn->despawn(live_.front());
                live_.pop_front();
            }
            const std::uint32_t copy =
                ctx.spawn->instance(template_, hand.hand_position, hand.hand_rotation);
            if (copy == kNone) continue;
            live_.push_back(copy);
            ++drawn_;
            ctx.hands->request_hold(h, copy);
            if (ctx.haptics != nullptr) ctx.haptics->pulse(h, 0.03f, 0.45f);
        }
    }

private:
    Vec3 source_point(MechanicContext& ctx) const {
        if (has_world_point_ || ctx.locomotion == nullptr) return world_point_;
        const Vec3 head = ctx.locomotion->head_position();
        const Quat hr = ctx.locomotion->head_rotation();
        Vec3 fwd = hr * Vec3(0.0f, 0.0f, -1.0f);
        fwd.y = 0.0f;
        if (glm::length(fwd) < 1e-4f) fwd = Vec3(0.0f, 0.0f, -1.0f);
        fwd = glm::normalize(fwd);
        const Vec3 right = glm::normalize(glm::cross(fwd, Vec3(0.0f, 1.0f, 0.0f)));
        const Vec3 o = slot_offset(slot_);
        return head + right * o.x + Vec3(0.0f, o.y, 0.0f) - fwd * o.z;
    }

    std::uint32_t template_{kNone};
    std::string slot_{"back"};
    float radius_{0.25f};
    int count_{0};
    int max_live_{12};
    int drawn_{0};
    Vec3 world_point_{0.0f};
    bool has_world_point_{false};
    std::deque<std::uint32_t> live_;
};

}  // namespace

extern "C" int ptv_force_link_builtin_item_source = 1;

PTV_REGISTER_MECHANIC("builtin.item_source", ItemSourceMechanic)

}  // namespace ptv
