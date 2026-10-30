// builtin.bow -- hold the bow, nock an arrow, draw, release.
//
// Bound to the bow. The bow is held by its grip socket like any item
// (builtin.grab); this mechanic owns the rest of the interaction:
//
//   1. NOCK: the OTHER hand brings an arrow (held, e.g. drawn from a quiver
//      by builtin.item_source, or picked up) within ``nock_radius_m`` of the
//      string's rest point (``string_rest`` socket) and squeezes. The held
//      arrow is consumed and the bow shows a nocked arrow of its own. With
//      ``allow_empty_draw`` an empty hand can draw a fresh arrow straight off
//      the string.
//   2. DRAW: while the drawing hand keeps squeezing, the nock follows it back
//      along the bow's rear axis up to ``max_draw_m``. The string is drawn
//      procedurally from the limb tips (``limb_tip_top`` / ``limb_tip_bottom``)
//      through the nock point -- generated bows often have no modelled
//      string, and a modelled one could not bend anyway -- and the haptics
//      tighten as the draw lengthens.
//   3. RELEASE: letting go launches the arrow through ``ctx.projectiles``
//      from the nock toward the arrow rest, at a speed proportional to the
//      draw, drawn with the arrow template's mesh, and sticking where it
//      lands. A draw shorter than ``min_draw_m`` just un-nocks.

#include <algorithm>
#include <cmath>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kGripPress = 0.7f;
constexpr float kGripRelease = 0.4f;

Quat look_rotation(const Vec3& from_dir, const Vec3& to_dir) {
    const Vec3 a = glm::normalize(from_dir);
    const Vec3 b = glm::normalize(to_dir);
    const float d = glm::dot(a, b);
    if (d > 0.99999f) return Quat(1, 0, 0, 0);
    if (d < -0.99999f) {
        Vec3 axis = glm::cross(Vec3(1, 0, 0), a);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(Vec3(0, 1, 0), a);
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    const Vec3 axis = glm::cross(a, b);
    return glm::normalize(Quat(1.0f + d, axis.x, axis.y, axis.z));
}

class BowMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.bow"; }
    MechanicPhase phase() const override { return MechanicPhase::Simulate; }

    void on_attach(World& world, std::uint32_t, const nlohmann::json& p,
                   MechanicContext&) override {
        max_draw_ = std::clamp(p.value("max_draw_m", 0.6f), 0.1f, 1.5f);
        min_draw_ = std::clamp(p.value("min_draw_m", 0.12f), 0.0f, max_draw_);
        max_speed_ = std::clamp(p.value("max_speed_mps", 70.0f), 5.0f, 500.0f);
        damage_ = std::max(0.0f, p.value("damage", 60.0f));
        nock_radius_ = std::clamp(p.value("nock_radius_m", 0.09f), 0.02f, 0.5f);
        allow_empty_draw_ = p.value("allow_empty_draw", true);
        arrow_asset_ = p.value("arrow_asset_id", std::string());
        string_color_ = Vec4(0.85f, 0.82f, 0.72f, 0.9f);
        const auto pj = p.value("projectile", nlohmann::json::object());
        proj_.gravity_scale = pj.value("gravity_scale", 1.0f);
        proj_.drag = pj.value("drag", 0.02f);
        proj_.radius_m = pj.value("radius_m", 0.006f);
        proj_.lifetime_s = pj.value("lifetime_s", 8.0f);
        proj_.stick_s = pj.value("stick_s", 8.0f);
        proj_.on_hit = ProjectileOnHit::Stick;
        proj_.impact = ImpactStyle::Dust;
        const std::string tmpl = p.value("arrow_template", std::string());
        if (!tmpl.empty()) {
            const auto e = world.find(tmpl);
            if (e != entt::null) arrow_template_ = static_cast<std::uint32_t>(e);
        }
        proj_.visual = arrow_template_ != kNone ? ProjectileVisual::Mesh : ProjectileVisual::Tracer;
        proj_.visual_template = arrow_template_;
        proj_.visual_width_m = 0.008f;
        proj_.visual_length_m = 0.7f;
        proj_.color = Vec4(0.75f, 0.6f, 0.4f, 1.0f);
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
                   MechanicContext& ctx) override {
        if (ctx.sockets == nullptr) return;
        int bow_hand = -1;
        for (int h = 0; h < 2; ++h) {
            if (input.hands[h].held_entity == self) bow_hand = h;
        }
        Vec3 top, bottom, rest, arrow_rest;
        if (!points(self, ctx, &top, &bottom, &rest, &arrow_rest)) return;
        // Rear axis: from the arrow rest back through the string.
        Vec3 back = rest - arrow_rest;
        if (glm::length(back) < 1e-4f) back = Vec3(ctx.sockets->socket_world(self, "string_rest")[2]) * -1.0f;
        back = glm::normalize(back);

        Vec3 nock = rest;
        if (bow_hand < 0) {
            nocked_ = false;
            draw_ = 0.0f;
        } else {
            const int dh = 1 - bow_hand;
            const auto& dhand = input.hands[dh];
            const bool pressed = dhand.prev_grip < kGripPress && dhand.grip >= kGripPress;
            const bool released = dhand.grip < kGripRelease;
            if (!nocked_ && pressed && dhand.hand_valid &&
                glm::length(dhand.hand_position - rest) <= nock_radius_) {
                try_nock(world, dh, input, ctx);
            }
            if (nocked_) {
                if (released || !dhand.hand_valid) {
                    if (draw_ >= min_draw_) release(world, self, bow_hand, dh, ctx, nock,
                                                    arrow_rest, rest, back);
                    nocked_ = false;
                    draw_ = 0.0f;
                    hide_nocked(world);
                } else {
                    draw_ = std::clamp(glm::dot(dhand.hand_position - rest, back), 0.0f, max_draw_);
                    nock = rest + back * draw_;
                    show_nocked(world, ctx, nock, glm::normalize(arrow_rest - nock));
                    haptic_clock_ += dt;
                    const float period = 0.12f - 0.08f * (draw_ / max_draw_);
                    if (ctx.haptics != nullptr && haptic_clock_ >= period) {
                        haptic_clock_ = 0.0f;
                        const float amp = 0.1f + 0.5f * (draw_ / max_draw_);
                        ctx.haptics->pulse(dh, 0.02f, amp);
                        ctx.haptics->pulse(bow_hand, 0.02f, amp * 0.5f);
                    }
                }
            }
        }
        // The string: limb tip -> nock -> limb tip.
        if (ctx.vfx != nullptr) {
            ctx.vfx->streak(top, nock, 0.004f, string_color_);
            ctx.vfx->streak(nock, bottom, 0.004f, string_color_);
        }
        publish(ctx, self, bow_hand);
    }

private:
    bool points(std::uint32_t self, MechanicContext& ctx, Vec3* top, Vec3* bottom, Vec3* rest,
                Vec3* arrow_rest) const {
        auto at = [&](const char* name, Vec3* out) {
            if (!ctx.sockets->has_socket(self, name)) return false;
            *out = Vec3(ctx.sockets->socket_world(self, name)[3]);
            return true;
        };
        if (!at("limb_tip_top", top) || !at("limb_tip_bottom", bottom)) return false;
        if (!at("string_rest", rest)) *rest = 0.5f * (*top + *bottom);
        if (!at("arrow_rest", arrow_rest)) {
            const Mat4 g = ctx.sockets->socket_world(self, "grip");
            *arrow_rest = Vec3(g[3]);
        }
        return true;
    }

    bool is_arrow(World& world, std::uint32_t e) const {
        if (e == kNone || !world.valid(e)) return false;
        if (has_tag(world, e, "arrow")) return true;
        if (!arrow_asset_.empty() && world.has<CMesh>(e) &&
            world.get<CMesh>(e).asset_id == arrow_asset_) {
            return true;
        }
        return false;
    }

    void try_nock(World& world, int dh, const InputSnapshot& input, MechanicContext& ctx) {
        const std::uint32_t held = input.hands[dh].held_entity;
        if (is_arrow(world, held)) {
            // The held arrow becomes the nocked one.
            if (ctx.spawn != nullptr) ctx.spawn->despawn(held);
        } else if (held != kNone || !allow_empty_draw_) {
            return;
        }
        nocked_ = true;
        draw_ = 0.0f;
        if (ctx.haptics != nullptr) ctx.haptics->pulse(dh, 0.03f, 0.5f);
    }

    void release(World&, std::uint32_t self, int bow_hand, int dh, MechanicContext& ctx,
                 const Vec3& nock, const Vec3& arrow_rest, const Vec3& rest, const Vec3& back) {
        const float k = draw_ / max_draw_;
        Vec3 dir = arrow_rest - nock;
        if (glm::length(dir) < 1e-4f) dir = -back;
        dir = glm::normalize(dir);
        ProjectileDesc pd = proj_;
        pd.origin = rest + dir * 0.02f;
        pd.velocity = dir * (max_speed_ * std::max(0.15f, k));
        pd.damage = damage_ * (0.25f + 0.75f * k);
        pd.shooter = ctx.hands != nullptr ? ctx.hands->player_entity() : kNone;
        pd.weapon = self;
        if (ctx.projectiles != nullptr) ctx.projectiles->launch(pd);
        ++fire_seq_;
        if (ctx.haptics != nullptr) {
            ctx.haptics->pulse(dh, 0.05f, 0.8f);
            ctx.haptics->pulse(bow_hand, 0.06f, 0.6f);
        }
    }

    void show_nocked(World& world, MechanicContext& ctx, const Vec3& nock, const Vec3& dir) {
        if (arrow_template_ == kNone || !world.valid(arrow_template_)) return;
        if (nocked_entity_ == kNone || !world.valid(nocked_entity_)) {
            const auto src = static_cast<entt::entity>(arrow_template_);
            if (!world.registry().all_of<CMesh>(src)) return;
            const auto dst = world.create_entity();
            const auto& mesh = world.registry().get<CMesh>(src);
            world.registry().emplace<CMesh>(dst, mesh);
            Transform t{};
            if (world.registry().all_of<CTransform>(src)) t = world.registry().get<CTransform>(src).t;
            world.registry().emplace<CTransform>(dst, CTransform{t});
            world.registry().emplace<CTag>(dst, CTag{{"nocked_arrow"}});
            nocked_entity_ = static_cast<std::uint32_t>(dst);
            const auto nk = mesh.sockets.find("nock");
            const auto ns = mesh.sockets.find("nose");
            nock_local_ = nk != mesh.sockets.end() ? nk->second.position * t.scale : Vec3(0.0f);
            if (ns != mesh.sockets.end()) {
                nose_dir_ = glm::normalize(ns->second.rotation * Vec3(0, 0, 1));
            }
        }
        (void)ctx;
        auto& t = world.get<CTransform>(nocked_entity_).t;
        auto& tag = world.registry().get_or_emplace<CTag>(static_cast<entt::entity>(nocked_entity_));
        tag.tags.erase(std::remove(tag.tags.begin(), tag.tags.end(), "template"), tag.tags.end());
        t.rotation = look_rotation(nose_dir_, dir);
        t.position = nock - t.rotation * nock_local_;
    }

    void hide_nocked(World& world) {
        if (nocked_entity_ == kNone || !world.valid(nocked_entity_)) return;
        auto& tag = world.registry().get_or_emplace<CTag>(static_cast<entt::entity>(nocked_entity_));
        if (std::find(tag.tags.begin(), tag.tags.end(), "template") == tag.tags.end()) {
            tag.tags.push_back("template");
        }
    }

    void publish(MechanicContext& ctx, std::uint32_t self, int hand) const {
        if (ctx.weapons == nullptr) return;
        WeaponState s{};
        s.configured = true;
        s.delivery = WeaponDelivery::Drawn;
        s.infinite = true;
        s.fire_seq = fire_seq_;
        s.held_by_hand = hand;
        s.reload_progress = draw_ / max_draw_;
        ctx.weapons->publish_state(self, s);
    }

    float max_draw_{0.6f};
    float min_draw_{0.12f};
    float max_speed_{70.0f};
    float damage_{60.0f};
    float nock_radius_{0.09f};
    bool allow_empty_draw_{true};
    std::string arrow_asset_;
    std::uint32_t arrow_template_{kNone};
    ProjectileDesc proj_{};
    Vec4 string_color_{0.85f, 0.82f, 0.72f, 0.9f};
    bool nocked_{false};
    float draw_{0.0f};
    float haptic_clock_{0.0f};
    std::uint32_t nocked_entity_{kNone};
    Vec3 nock_local_{0.0f};
    Vec3 nose_dir_{0.0f, 0.0f, 1.0f};
    std::uint64_t fire_seq_{0};
};

}  // namespace

extern "C" int ptv_force_link_builtin_bow = 1;

PTV_REGISTER_MECHANIC("builtin.bow", BowMechanic)

}  // namespace ptv
