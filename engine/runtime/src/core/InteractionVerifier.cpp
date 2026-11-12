#include "InteractionVerifier.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "ptv/api/Facing.h"
#include "ptv/api/GameFlow.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kSettleFirstS = 0.5f;   // positions recorded
constexpr float kSettleS = 3.5f;        // ... and compared
constexpr std::size_t kMaxProps = 16;
constexpr std::size_t kMaxExplosives = 2;
constexpr std::size_t kMaxViews = 10;

Quat look_along(const Vec3& dir) {
    Vec3 d = glm::length(dir) > 1e-5f ? glm::normalize(dir) : Vec3(0.0f, 0.0f, -1.0f);
    Vec3 up(0.0f, 1.0f, 0.0f);
    if (std::fabs(glm::dot(d, up)) > 0.98f) up = Vec3(0.0f, 0.0f, 1.0f);
    return glm::quatLookAtRH(d, up);
}

std::string name_of(World& world, std::uint32_t e) {
    const std::string* n = world.id_of(static_cast<entt::entity>(e));
    return n != nullptr ? *n : std::to_string(e);
}

Vec3 flat(const Vec3& v) {
    const Vec3 f(v.x, 0.0f, v.z);
    return glm::length(f) > 1e-4f ? glm::normalize(f) : Vec3(0.0f, 0.0f, -1.0f);
}

float smooth(float a, float b, float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);
    return a + (b - a) * s * s * (3.0f - 2.0f * s);
}

}  // namespace

void InteractionVerifier::next(Phase p) {
    phase_ = p;
    phase_t_ = 0.0f;
    index_ = 0;
}

Vec3 InteractionVerifier::where(World& world, MechanicContext& ctx, std::uint32_t e) const {
    Vec3 c = world.has<CTransform>(e) ? world.get<CTransform>(e).t.position : Vec3(0.0f);
    if (ctx.physics != nullptr) ctx.physics->body_center(e, &c);
    return c;
}

float InteractionVerifier::floor_below(MechanicContext& ctx, std::uint32_t self,
                                       const Vec3& p) const {
    if (ctx.physics == nullptr) return p.y;
    PhysicsRayHit hit{};
    if (ctx.physics->raycast_floor(self, p + Vec3(0.0f, 0.05f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                   6.0f, hit)) {
        return hit.point.y;
    }
    return p.y;
}

void InteractionVerifier::collect(World& world, MechanicContext& ctx) {
    collected_ = true;
    const std::uint32_t player = ctx.hands != nullptr ? ctx.hands->player_entity() : kNone;
    auto view = world.registry().view<CMechanicBinding>();
    for (const auto ent : view) {
        const auto e = static_cast<std::uint32_t>(ent);
        if (e == player || has_tag(world, e, "template") || world.has<CAttachment>(e)) continue;
        if (has_tag(world, e, "player_rig") || has_tag(world, e, "player_system")) continue;
        for (const auto& b : view.get<CMechanicBinding>(ent).entries) {
            const std::string& id = b.builtin_id.empty() ? b.id : b.builtin_id;
            const auto p = nlohmann::json::parse(b.params_json, nullptr, false);
            Item it{};
            it.entity = e;
            it.name = name_of(world, e);
            if (id == "builtin.grab" && world.has<CPhysics>(e) &&
                world.get<CPhysics>(e).kind == "dynamic" && props_.size() < kMaxProps) {
                props_.push_back(it);
            } else if (id == "builtin.press_button") {
                it.signal = p.is_object() ? p.value("signal", it.name + ".pressed")
                                          : it.name + ".pressed";
                buttons_.push_back(it);
            } else if (id == "builtin.target_challenge") {
                it.challenge = p.is_object() ? p.value("id", it.name) : it.name;
                it.signal = p.is_object() ? p.value("start_signal", std::string()) : "";
                challenges_.push_back(it);
            } else if (id == "builtin.explosive" && explosives_.size() < kMaxExplosives) {
                explosives_.push_back(it);
                if (p.is_object()) fuse_s_ = std::max(fuse_s_, p.value("fuse_s", 3.0f));
            }
        }
    }
    // Height above the floor the PLAYER stands on (a gun on a table is
    // 0.8 m up, not 9 cm above the table top it rests on).
    const float ground = ctx.locomotion != nullptr ? ctx.locomotion->body_position().y : 0.0f;
    for (auto& it : props_) {
        it.start = where(world, ctx, it.entity);
        it.height_above_floor = it.start.y - std::min(ground, floor_below(ctx, it.entity, it.start));
        if (world.has<CTransform>(it.entity)) {
            it.home_pos = world.get<CTransform>(it.entity).t.position;
            it.home_rot = world.get<CTransform>(it.entity).t.rotation;
        }
    }
    if (viewpoints_.size() > kMaxViews) viewpoints_.resize(kMaxViews);
    PTV_INFO("interact: begin props={} buttons={} challenges={} explosives={} viewpoints={}",
             props_.size(), buttons_.size(), challenges_.size(), explosives_.size(),
             viewpoints_.size());
}

void InteractionVerifier::stand_facing(World& world, MechanicContext& ctx, const Vec3& target,
                                       float dist) {
    if (ctx.app == nullptr) return;
    const Vec3 body = ctx.locomotion != nullptr ? ctx.locomotion->body_position() : target;
    const float ground = body.y;
    Vec3 away = flat(body - target);
    // A spot on the player's own floor (not on the table the item is on)
    // with nothing solid between the eyes and the item.
    for (int k = 0; k < 8; ++k) {
        const Quat yaw = glm::angleAxis(glm::radians(45.0f * static_cast<float>((k + 1) / 2) *
                                                     (k % 2 == 0 ? 1.0f : -1.0f)),
                                        Vec3(0.0f, 1.0f, 0.0f));
        const Vec3 d = yaw * away;
        Vec3 feet = target + d * dist;
        feet.y = floor_below(ctx, kNone, Vec3(feet.x, ground + 1.0f, feet.z));
        if (std::fabs(feet.y - ground) > 0.35f) continue;
        bool clear = true;
        if (ctx.physics != nullptr) {
            const Vec3 eye = feet + Vec3(0.0f, 1.5f, 0.0f);
            const Vec3 to = target - eye;
            const float len = glm::length(to);
            PhysicsRayHit hit{};
            clear = len < 0.2f || !ctx.physics->raycast_floor(kNone, eye, to / len, len - 0.15f, hit);
        }
        if (!clear) continue;
        ctx.app->place_player(feet, yaw_from_forward(flat(target - feet)));
        return;
    }
    Vec3 feet = target + away * dist;
    feet.y = ground;
    ctx.app->place_player(feet, yaw_from_forward(flat(target - feet)));
    (void)world;
}

void InteractionVerifier::tick(float dt, World& world, MechanicContext& ctx,
                               const Vec3& real_head, const Quat& head_rot) {
    if (phase_ == Phase::Done || dt <= 0.0f) return;
    t_ += dt;
    phase_t_ += dt;
    real_head_ = real_head;
    for (auto& h : hands_) {
        h.prev_trigger = h.trigger;
        h.prev_grip = h.grip;
    }
    // A headset on a desk can report its head anywhere; the scripted player
    // has its eyes 1.6 m above its feet.
    Vec3 head = real_head;
    if (ctx.locomotion != nullptr) {
        head = ctx.locomotion->body_position() + Vec3(0.0f, kEyeHeightM, 0.0f);
    }

    switch (phase_) {
        case Phase::Settle: {
            if (!collected_ && t_ >= kSettleFirstS) collect(world, ctx);
            if (t_ < kSettleS) return;
            for (const auto& it : props_) {
                if (!world.valid(it.entity)) continue;
                const float moved = glm::length(where(world, ctx, it.entity) - it.start) * 100.0f;
                PTV_INFO("interact: settle e={} moved_cm={:.2f} height_m={:.2f}", it.name, moved,
                         it.height_above_floor);
            }
            next(Phase::Walk);
            return;
        }
        case Phase::Walk:
            tick_walk(dt, world, ctx);
            return;
        case Phase::Grab:
            tick_grab(world, ctx, head);
            return;
        case Phase::Press:
            tick_press(world, ctx, head);
            return;
        case Phase::Challenge:
            tick_challenge(world, ctx);
            return;
        case Phase::Throw:
            tick_throw(world, ctx, head);
            return;
        case Phase::Views:
            tick_views(world, ctx, head_rot);
            return;
        default:
            return;
    }
}

void InteractionVerifier::tick_walk(float dt, World& world, MechanicContext& ctx) {
    for (auto& h : hands_) {
        h.active = true;
        h.valid = false;  // arms down: only the body walks into it
    }
    if (index_ >= props_.size()) {
        next(Phase::Grab);
        return;
    }
    const Item& it = props_[index_];
    if (!world.valid(it.entity)) {
        ++index_;
        phase_t_ = 0.0f;
        return;
    }
    if (phase_t_ <= dt) stand_facing(world, ctx, where(world, ctx, it.entity), 1.0f);
    if (phase_t_ < 0.4f) {
        mark_ = where(world, ctx, it.entity);
        if (ctx.locomotion != nullptr) dir_ = flat(mark_ - ctx.locomotion->body_position());
        return;
    }
    if (phase_t_ < 1.8f) {
        if (ctx.locomotion != nullptr) ctx.locomotion->move_horizontal(dir_ * (1.4f * dt));
        return;
    }
    const float moved = glm::length(where(world, ctx, it.entity) - mark_) * 100.0f;
    PTV_INFO("interact: walk e={} moved_cm={:.2f} height_m={:.2f}", it.name, moved,
             it.height_above_floor);
    ++index_;
    phase_t_ = 0.0f;
}

void InteractionVerifier::tick_grab(World& world, MechanicContext& ctx, const Vec3& head) {
    hands_[0] = HandScript{};
    hands_[0].active = true;
    hands_[0].valid = false;
    auto& h = hands_[1];
    if (index_ >= props_.size()) {
        h = HandScript{};
        next(Phase::Press);
        return;
    }
    const Item& it = props_[index_];
    if (!world.valid(it.entity)) {
        ++index_;
        phase_t_ = 0.0f;
        return;
    }
    if (phase_t_ < 0.05f) {
        stand_facing(world, ctx, where(world, ctx, it.entity), 0.5f);
        flag_ = false;
        pushed_ = 0.0f;
    }
    const Vec3 target = phase_t_ < 0.3f ? where(world, ctx, it.entity) : mark_;
    const Vec3 toward = flat(target - head);
    const Vec3 right = glm::normalize(glm::cross(toward, Vec3(0.0f, 1.0f, 0.0f)));
    const Vec3 rest = head - Vec3(0.0f, 0.45f, 0.0f) + toward * 0.15f + right * 0.12f;
    h.active = true;
    h.valid = true;
    h.rot = look_along(toward);
    h.trigger = 0.0f;
    if (phase_t_ < 0.3f) {
        mark_ = target;
        hand_from_ = rest;
        h.pos = rest;
        h.grip = 0.0f;
        return;
    }
    const Vec3 at = mark_ + Vec3(0.0f, 0.03f, 0.0f) - toward * 0.04f;
    if (phase_t_ < 1.1f) {
        const float s = (phase_t_ - 0.3f) / 0.8f;
        h.pos = Vec3(smooth(hand_from_.x, at.x, s), smooth(hand_from_.y, at.y, s),
                     smooth(hand_from_.z, at.z, s));
        h.grip = 0.0f;
        pushed_ = std::max(pushed_, glm::length(where(world, ctx, it.entity) - mark_) * 100.0f);
        return;
    }
    if (phase_t_ < 1.6f) {
        h.pos = at;
        h.grip = 1.0f;
        return;
    }
    if (!flag_ && phase_t_ < 1.65f) {
        flag_ = ctx.hands != nullptr && ctx.hands->held_entity(1) == it.entity;
    }
    if (phase_t_ < 2.1f) {
        h.pos = at + Vec3(0.0f, 0.25f * std::clamp((phase_t_ - 1.6f) / 0.5f, 0.0f, 1.0f), 0.0f);
        h.grip = 1.0f;
        return;
    }
    if (phase_t_ < 2.15f) {
        var0_ = (where(world, ctx, it.entity).y - mark_.y) * 100.0f;  // lifted
        flag_restored_ = false;
    }
    h.grip = 0.0f;
    // Released, still falling: back where it started, before it lands on
    // its neighbour -- a dropped rifle once knocked the blaster beside it
    // 5.6 cm, charged to the blaster's own reach.
    if (!flag_restored_ && phase_t_ >= 2.25f) {
        flag_restored_ = true;
        if (ctx.physics != nullptr && world.has<CTransform>(it.entity) &&
            (ctx.hands == nullptr || ctx.hands->held_entity(1) != it.entity)) {
            auto& t = world.get<CTransform>(it.entity).t;
            t.position = it.home_pos;
            t.rotation = it.home_rot;
            ctx.physics->teleport_body(it.entity, it.home_pos, it.home_rot);
        }
    }
    if (phase_t_ < 2.7f) return;
    PTV_INFO("interact: grab e={} held={} pushed_cm={:.2f} lifted_cm={:.1f} height_m={:.2f}",
             it.name, flag_ ? 1 : 0, pushed_, var0_, it.height_above_floor);
    ++index_;
    phase_t_ = 0.0f;
}

void InteractionVerifier::tick_press(World& world, MechanicContext& ctx, const Vec3& head) {
    hands_[0] = HandScript{};
    hands_[0].active = true;
    hands_[0].valid = false;
    auto& h = hands_[1];
    if (index_ >= buttons_.size()) {
        h = HandScript{};
        next(Phase::Challenge);
        return;
    }
    const Item& it = buttons_[index_];
    if (!world.valid(it.entity)) {
        ++index_;
        phase_t_ = 0.0f;
        return;
    }
    // The top of the cap: the collider's top face, at its centre.
    Vec3 cap = where(world, ctx, it.entity) + Vec3(0.0f, 0.05f, 0.0f);
    if (world.has<CCollisionGeom>(it.entity) && world.has<CTransform>(it.entity)) {
        const auto& g = world.get<CCollisionGeom>(it.entity);
        const auto& t = world.get<CTransform>(it.entity).t;
        const Vec3 local((g.aabb_min.x + g.aabb_max.x) * 0.5f, g.aabb_max.y,
                         (g.aabb_min.z + g.aabb_max.z) * 0.5f);
        cap = t.position + t.rotation * (local * t.scale);
    }
    const std::uint32_t count = ctx.signals != nullptr ? ctx.signals->count(it.signal.c_str()) : 0;
    if (phase_t_ < 0.05f) {
        stand_facing(world, ctx, cap, 0.45f);
        count0_ = count;
        how_.clear();
        have_tip_offset_ = false;
    }
    const Vec3 toward = flat(cap - head);
    h.active = true;
    h.valid = true;
    h.rot = look_along(toward + Vec3(0.0f, -1.0f, 0.0f));
    h.grip = 0.0f;
    if (how_.empty() && count > count0_) how_ = phase_t_ < 1.65f ? "poke" : "trigger";
    if (!how_.empty() || phase_t_ >= 2.6f) {
        h.trigger = 0.0f;
        // Where the fingertip and the drawn hand really were, for a press
        // that did not land.
        Vec3 tip(0.0f);
        const bool tip_ok = ctx.hands != nullptr && ctx.hands->fingertip_world(1, "index", &tip);
        float hand_cm = -1.0f;
        if (ctx.hands != nullptr) {
            const std::uint32_t he = ctx.hands->hand_entity(1);
            if (he != kNone && world.valid(he) && world.has<CTransform>(he)) {
                hand_cm = glm::length(world.get<CTransform>(he).t.position - cap) * 100.0f;
            }
        }
        PTV_INFO("interact: press e={} signal={} by={} tip={} tip_cm={:.1f} hand_cm={:.1f} "
                 "script_cm={:.1f} held={}",
                 it.name, it.signal, how_.empty() ? "none" : how_, have_tip_offset_ ? 1 : 0,
                 tip_ok ? glm::length(tip - cap) * 100.0f : -1.0f, hand_cm,
                 glm::length(h.pos - cap) * 100.0f,
                 ctx.hands != nullptr && ctx.hands->held_entity(1) != kNone ? 1 : 0);
        if (!how_.empty()) pressed_signals_.push_back(it.signal);
        ++index_;
        phase_t_ = 0.0f;
        return;
    }
    if (phase_t_ < 0.5f) {
        h.pos = cap + Vec3(0.0f, 0.12f, 0.0f) - toward * 0.05f;
        h.trigger = 0.0f;
        Vec3 tip(0.0f);
        if (phase_t_ > 0.3f) {
            // The point builtin.press_button pokes with: the drawn fingertip,
            // or the controller's poke point when the drawn one trails it.
            const Vec3 poke_point = h.pos + h.rot * Vec3(0.0f, -0.01f, -0.095f);
            if (ctx.hands == nullptr || !ctx.hands->fingertip_world(1, "index", &tip) ||
                glm::length(tip - poke_point) > 0.08f) {
                tip = poke_point;
            }
            tip_offset_ = glm::inverse(h.rot) * (tip - h.pos);
            have_tip_offset_ = glm::length(tip_offset_) < 0.3f;
        }
        return;
    }
    if (phase_t_ < 1.6f) {
        // The fingertip from 6 cm above the cap to 2 cm into it.
        const float s = (phase_t_ - 0.5f) / 1.0f;
        const Vec3 tip_at = cap + Vec3(0.0f, smooth(0.06f, -0.02f, s), 0.0f);
        h.pos = have_tip_offset_ ? tip_at - h.rot * tip_offset_ : tip_at;
        h.trigger = 0.0f;
        return;
    }
    // No poke: the trigger beside it, as a controller player would.
    h.pos = cap + Vec3(0.0f, 0.05f, 0.0f) - toward * 0.03f;
    h.trigger = (phase_t_ > 1.9f && phase_t_ < 2.1f) ? 1.0f : 0.0f;
}

void InteractionVerifier::tick_challenge(World& world, MechanicContext& ctx) {
    hands_[0] = HandScript{};
    hands_[1] = HandScript{};
    if (index_ >= challenges_.size()) {
        next(Phase::Throw);
        return;
    }
    const Item& it = challenges_[index_];
    const bool started = it.signal.empty() ||
                         std::find(pressed_signals_.begin(), pressed_signals_.end(), it.signal) !=
                             pressed_signals_.end();
    const std::string state = ctx.vars != nullptr ? ctx.vars->text((it.challenge + ".state").c_str())
                                                  : std::string();
    const float left = ctx.vars != nullptr ? ctx.vars->number((it.challenge + ".time_left").c_str())
                                           : 0.0f;
    if (phase_t_ < 0.05f) {
        flag_ = false;
        var0_ = -1.0f;
    }
    if (state == "running" && !flag_) {
        flag_ = true;
        var0_ = left;
        mark_ = Vec3(phase_t_, 0.0f, 0.0f);
    }
    const bool settled = flag_ && phase_t_ - mark_.x > 1.5f;
    if (!settled && phase_t_ < 9.0f) return;
    int active = 0;
    auto view = world.registry().view<CTag, CTransform>();
    for (const auto ent : view) {
        const auto e = static_cast<std::uint32_t>(ent);
        if (has_tag(world, e, "challenge_target") && !has_tag(world, e, "template")) ++active;
    }
    PTV_INFO("interact: challenge id={} signal={} pressed={} state={} active={} "
             "time_left_drop={:.2f}",
             it.challenge, it.signal.empty() ? "-" : it.signal, started ? 1 : 0,
             state.empty() ? "none" : state, active, flag_ ? var0_ - left : 0.0f);
    ++index_;
    phase_t_ = 0.0f;
}

void InteractionVerifier::tick_throw(World& world, MechanicContext& ctx, const Vec3& head) {
    hands_[0] = HandScript{};
    hands_[0].active = true;
    hands_[0].valid = false;
    auto& h = hands_[1];
    if (index_ >= explosives_.size()) {
        h = HandScript{};
        next(Phase::Views);
        return;
    }
    const Item& it = explosives_[index_];
    if (!world.valid(it.entity)) {
        ++index_;
        phase_t_ = 0.0f;
        return;
    }
    const std::string sig = it.name + ".detonated";
    const std::uint32_t det = ctx.signals != nullptr ? ctx.signals->count(sig.c_str()) : 0;
    if (phase_t_ < 0.05f) {
        stand_facing(world, ctx, where(world, ctx, it.entity), 0.6f);
        count0_ = det;
        flag_ = false;
        var0_ = ctx.vars != nullptr ? ctx.vars->number("destructibles.destroyed") : 0.0f;
        // Throw at the nearest destructible, else downrange.
        dir_ = Vec3(0.0f);
        float best = 25.0f;
        auto view = world.registry().view<CMechanicBinding, CTransform>();
        for (const auto ent : view) {
            const auto e = static_cast<std::uint32_t>(ent);
            if (has_tag(world, e, "template")) continue;
            for (const auto& b : view.get<CMechanicBinding>(ent).entries) {
                const std::string& id = b.builtin_id.empty() ? b.id : b.builtin_id;
                if (id != "builtin.destructible") continue;
                const Vec3 p = where(world, ctx, e);
                const float d = glm::length(p - head);
                if (d > 3.0f && d < best) {
                    best = d;
                    dir_ = p - head;
                }
            }
        }
        if (glm::length(dir_) < 1e-3f) dir_ = flat(where(world, ctx, it.entity) - head) * 6.0f;
    }
    const Vec3 toward = flat(dir_);
    const Vec3 right = glm::normalize(glm::cross(toward, Vec3(0.0f, 1.0f, 0.0f)));
    h.active = true;
    h.valid = true;
    h.rot = look_along(toward);
    if (phase_t_ < 0.3f) {
        h.pos = where(world, ctx, it.entity);
        h.grip = 0.0f;
        return;
    }
    const Vec3 ready = head - Vec3(0.0f, 0.3f, 0.0f) + right * 0.18f - toward * 0.1f;
    if (phase_t_ < 0.35f && ctx.hands != nullptr) ctx.hands->request_hold(1, it.entity);
    if (phase_t_ < 1.6f) {
        h.grip = 1.0f;
        h.pos = ready;
        h.trigger = (phase_t_ > 1.1f && phase_t_ < 1.3f) ? 1.0f : 0.0f;  // the pin
        if (phase_t_ > 1.0f && !flag_) {
            flag_ = ctx.hands != nullptr && ctx.hands->held_entity(1) == it.entity;
        }
        hand_from_ = ready;
        return;
    }
    if (phase_t_ < 1.85f) {
        // The throw: up and out at ~8 m/s; the engine's hand velocity (and
        // so builtin.throw) follows the scripted motion.
        const Vec3 launch = glm::normalize(toward + Vec3(0.0f, 0.55f, 0.0f));
        h.grip = 1.0f;
        h.trigger = 0.0f;
        h.pos = hand_from_ + launch * (8.0f * (phase_t_ - 1.6f));
        return;
    }
    h.grip = 0.0f;
    if (phase_t_ < 1.9f) return;
    h.valid = false;
    const bool detonated = det > count0_;
    if (!detonated && phase_t_ < 1.9f + fuse_s_ + 2.5f) return;
    const float destroyed =
        (ctx.vars != nullptr ? ctx.vars->number("destructibles.destroyed") : 0.0f) - var0_;
    PTV_INFO("interact: explosive e={} held={} detonated={} destroyed={:.0f}", it.name,
             flag_ ? 1 : 0, detonated ? 1 : 0, destroyed);
    ++index_;
    phase_t_ = 0.0f;
}

void InteractionVerifier::tick_views(World& world, MechanicContext& ctx, const Quat& head_rot) {
    for (auto& h : hands_) {
        h.active = true;
        h.valid = false;
    }
    // First, the player's own hands, 35 cm in front of the REAL eye: the
    // body-part colliders once drew as white boxes over them and no check
    // looked at the player's own body.
    if (!self_view_done_) {
        if (phase_t_ < 0.05f) flag_ = false;
        const Vec3 fwd = head_rot * Vec3(0.0f, 0.0f, -1.0f);
        const Vec3 up = head_rot * Vec3(0.0f, 1.0f, 0.0f);
        const Vec3 right = head_rot * Vec3(1.0f, 0.0f, 0.0f);
        for (int h = 0; h < 2; ++h) {
            auto& s = hands_[h];
            s.valid = true;
            s.pos = real_head_ + fwd * 0.35f - up * 0.12f + right * (h == 0 ? -0.1f : 0.1f);
            s.rot = look_along(fwd);
            s.grip = 0.0f;
            s.trigger = 0.0f;
        }
        if (phase_t_ >= 1.5f && !flag_) {
            flag_ = true;
            PTV_INFO("interact: viewpoint id=self_view index=-1");
        }
        if (phase_t_ < 4.0f) return;
        self_view_done_ = true;
        flag_ = false;
        phase_t_ = 0.0f;
        return;
    }
    if (index_ >= viewpoints_.size()) {
        hands_[0] = HandScript{};
        hands_[1] = HandScript{};
        phase_ = Phase::Done;
        PTV_INFO("interact: done");
        return;
    }
    const VerifyViewpoint& v = viewpoints_[index_];
    if (phase_t_ < 0.05f && ctx.app != nullptr) {
        Vec3 feet = v.eye - Vec3(0.0f, 1.6f, 0.0f);
        feet.y = floor_below(ctx, kNone, v.eye);
        // Turn the rig so the HEAD (wherever it points on its own) faces
        // the target.
        const float want = yaw_from_forward(flat(v.target - v.eye));
        const float head_yaw = yaw_from_forward(flat(head_rot * Vec3(0.0f, 0.0f, -1.0f)));
        const float rig_yaw = ctx.locomotion != nullptr ? ctx.locomotion->yaw_radians() : 0.0f;
        view_feet_ = feet;
        view_yaw_ = want - (head_yaw - rig_yaw);
        view_fixed_ = false;
        flag_ = false;  // the throw phase left it set: the spawn view was never shot
        ctx.app->place_player(view_feet_, view_yaw_);
    }
    // The rig went to ``feet``; the real headset sits wherever it sits in
    // the play area (a metre off on a desk is common), which put the camera
    // through a wall. Move the rig by the eye's error once it is measurable.
    if (!view_fixed_ && phase_t_ >= 0.25f && ctx.app != nullptr) {
        view_fixed_ = true;
        const Vec3 err(v.eye.x - real_head_.x, 0.0f, v.eye.z - real_head_.z);
        if (glm::length(err) > 0.05f) {
            view_feet_ += err;
            ctx.app->place_player(view_feet_, view_yaw_);
        }
    }
    if (phase_t_ >= 1.5f && !flag_) {
        flag_ = true;
        PTV_INFO("interact: viewpoint id={} index={}", v.id, index_);
    }
    if (phase_t_ < 4.0f) return;
    flag_ = false;
    ++index_;
    phase_t_ = 0.0f;
    (void)world;
}

bool InteractionVerifier::scripted_pose(int h, Vec3* pos, Quat* rot, bool* valid) const {
    if (phase_ == Phase::Done || phase_ == Phase::Settle || h < 0 || h > 1) return false;
    const HandScript& hs = hands_[h];
    if (!hs.active) return false;
    *pos = hs.pos;
    *rot = hs.rot;
    *valid = hs.valid;
    return true;
}

void InteractionVerifier::patch(InputSnapshot& s) const {
    if (phase_ == Phase::Done || phase_ == Phase::Settle) return;
    for (int h = 0; h < 2; ++h) {
        const HandScript& hs = hands_[h];
        if (!hs.active) continue;
        auto& out = s.hands[h];
        out.hand_valid = hs.valid;
        out.aim_valid = hs.valid;
        out.trigger = hs.valid ? hs.trigger : 0.0f;
        out.prev_trigger = hs.valid ? hs.prev_trigger : 0.0f;
        out.grip = hs.valid ? hs.grip : 0.0f;
        out.prev_grip = hs.valid ? hs.prev_grip : 0.0f;
    }
}

}  // namespace ptv
