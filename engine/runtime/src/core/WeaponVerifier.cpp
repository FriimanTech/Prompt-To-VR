#include "WeaponVerifier.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "gameplay/CombatRules.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr float kSettleS = 4.0f;
constexpr float kHoldS = 1.2f;
constexpr float kSemiS = 2.5f;    // five pulls, 0.5 s apart
constexpr float kAutoS = 3.0f;
constexpr float kBeamS = 2.0f;
constexpr float kBowS = 2.2f;     // nock, draw 1 s, hold, release
constexpr float kReleaseS = 0.8f;

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

const char* delivery_name(WeaponDelivery d) {
    switch (d) {
        case WeaponDelivery::Hitscan: return "hitscan";
        case WeaponDelivery::Projectile: return "projectile";
        case WeaponDelivery::Hybrid: return "hybrid";
        case WeaponDelivery::Beam: return "beam";
        case WeaponDelivery::Drawn: return "drawn";
        default: return "other";
    }
}

}  // namespace

void WeaponVerifier::collect(World& world) {
    weapons_.clear();
    auto view = world.registry().view<CMechanicBinding, CMesh>();
    for (const auto ent : view) {
        const auto e = static_cast<std::uint32_t>(ent);
        if (has_tag(world, e, "template")) continue;
        // NPC gear and characters modelled with their gun are not the player's.
        if (world.has<CAttachment>(e) || world.has<CFaction>(e)) continue;
        for (const auto& b : view.get<CMechanicBinding>(ent).entries) {
            const std::string& id = b.builtin_id.empty() ? b.id : b.builtin_id;
            if (id != "builtin.weapon" && id != "builtin.bow") continue;
            Weapon w{};
            w.entity = e;
            w.name = name_of(world, e);
            w.bow = id == "builtin.bow";
            const auto p = nlohmann::json::parse(b.params_json, nullptr, false);
            if (p.is_object()) {
                w.muzzle = p.value("muzzle_socket", std::string("muzzle"));
                const auto pj = p.value("projectile", nlohmann::json::object());
                if (w.bow) {
                    // Released at full draw by the script.
                    w.speed_mps = p.value("max_speed_mps", 70.0f);
                    w.gravity_scale = pj.value("gravity_scale", 1.0f);
                } else if (p.value("delivery", std::string()) == "projectile") {
                    w.speed_mps = pj.value("speed_mps", 360.0f);
                    w.gravity_scale = pj.value("gravity_scale", 1.0f);
                }
            }
            weapons_.push_back(std::move(w));
            break;
        }
    }
    PTV_INFO("weapons: begin count={}", weapons_.size());
}

std::uint32_t WeaponVerifier::find_target(World& world, MechanicContext& ctx,
                                          const Vec3& head_pos, const Vec3& fwd) {
    const std::uint32_t player = ctx.hands != nullptr ? ctx.hands->player_entity() : kNone;
    // A close hostile is a fair target; one across the map is a moving dot
    // an unattended script cannot be expected to lead.
    if (ctx.perception != nullptr && player != kNone) {
        const std::uint32_t t = ctx.perception->nearest_hostile(player, 15.0f);
        if (t != kNone && world.valid(t)) return t;
    }
    if (spawned_ != kNone && world.valid(spawned_) &&
        (ctx.combat == nullptr || ctx.combat->is_alive(spawned_))) {
        return spawned_;
    }
    // A dummy: a clone of the first NPC template, ~8 m away in a direction
    // with a clear line of sight (8 m straight ahead may be inside a cliff).
    if (ctx.spawn != nullptr) {
        Vec3 flat(fwd.x, 0.0f, fwd.z);
        flat = glm::length(flat) > 1e-4f ? glm::normalize(flat) : Vec3(0, 0, -1);
        float best = 0.0f;
        Vec3 best_dir = flat;
        for (int k = 0; k < 8 && ctx.physics != nullptr; ++k) {
            const Quat yaw = glm::angleAxis(glm::radians(45.0f * static_cast<float>(k)),
                                            Vec3(0.0f, 1.0f, 0.0f));
            const Vec3 d = yaw * flat;
            PhysicsRayHit h{};
            const std::uint32_t ignore[1] = {player};
            const float clear = ctx.physics->raycast_ignoring(ignore, 1, head_pos, d, 9.0f, h)
                                    ? glm::length(h.point - head_pos) : 9.0f;
            if (clear > best + 0.5f) {
                best = clear;
                best_dir = d;
            }
            if (clear >= 9.0f) break;
        }
        const float dist = ctx.physics != nullptr ? std::clamp(best - 1.0f, 3.0f, 8.0f) : 8.0f;
        auto view = world.registry().view<CFaction, CMesh>();
        for (const auto ent : view) {
            const auto e = static_cast<std::uint32_t>(ent);
            if (!has_tag(world, e, "template")) continue;
            const Vec3 at(head_pos.x + best_dir.x * dist, 0.0f, head_pos.z + best_dir.z * dist);
            // At the template's own height: its origin may be at its feet or
            // at its middle, and character locomotion grounds it either way.
            const float y = world.has<CTransform>(e) ? world.get<CTransform>(e).t.position.y
                                                     : head_pos.y - 1.0f;
            spawned_ = ctx.spawn->instance(e, Vec3(at.x, std::max(y, head_pos.y - 1.6f), at.z),
                                           Quat(1.0f, 0.0f, 0.0f, 0.0f));
            if (spawned_ != kNone) {
                PTV_INFO("weapons: dummy spawned template={} e={}", name_of(world, e), spawned_);
                return spawned_;
            }
            break;
        }
    }
    return kNone;
}

void WeaponVerifier::tick(float dt, World& world, MechanicContext& ctx, const Vec3& real_head,
                          const Quat& head_rot, int max_live_projectiles) {
    if (phase_ == Phase::Done || dt <= 0.0f) return;
    // A headset lying on a desk can report a head at knee height or below
    // the floor; the scripted player stands with eyes at 1.6 m.
    Vec3 head_pos = real_head;
    if (ctx.locomotion != nullptr) {
        const Vec3 feet = ctx.locomotion->body_position();
        head_pos = Vec3(feet.x, feet.y + 1.6f, feet.z);
    }
    t_ += dt;
    phase_t_ += dt;
    for (auto& h : hands_) {
        h.prev_trigger = h.trigger;
        h.prev_grip = h.grip;
    }
    Vec3 fwd = head_rot * Vec3(0.0f, 0.0f, -1.0f);
    Vec3 flat(fwd.x, 0.0f, fwd.z);
    flat = glm::length(flat) > 1e-4f ? glm::normalize(flat) : Vec3(0, 0, -1);
    const Vec3 right = glm::normalize(glm::cross(flat, Vec3(0.0f, 1.0f, 0.0f)));

    if (phase_ == Phase::Settle) {
        if (t_ < kSettleS) return;
        collect(world);
        index_ = 0;
        phase_ = weapons_.empty() ? Phase::Done : Phase::Pick;
        phase_t_ = 0.0f;
        if (weapons_.empty()) PTV_INFO("weapons: done count=0");
        return;
    }
    if (index_ >= weapons_.size()) {
        phase_ = Phase::Done;
        hands_[0] = HandScript{};
        hands_[1] = HandScript{};
        PTV_INFO("weapons: done count={}", weapons_.size());
        return;
    }
    const Weapon& w = weapons_[index_];
    if (!world.valid(w.entity)) {
        ++index_;
        phase_ = Phase::Pick;
        phase_t_ = 0.0f;
        return;
    }
    const int gun_hand = w.bow ? 0 : 1;
    const int other = 1 - gun_hand;

    // Where to aim: the target's chest, else 8 m ahead.
    target_ = find_target(world, ctx, head_pos, flat);
    Vec3 aim_point = head_pos + flat * 8.0f - Vec3(0.0f, 0.3f, 0.0f);
    if (target_ != kNone && world.has<CTransform>(target_)) {
        aim_point = combat_rules::aim_point(world, target_, ctx.physics);
    }
    auto& gh = hands_[gun_hand];
    gh.active = true;
    gh.pos = head_pos + flat * (w.bow ? 0.55f : 0.38f) - Vec3(0.0f, w.bow ? 0.12f : 0.24f, 0.0f) +
             right * (gun_hand == 1 ? 0.16f : -0.05f);
    if (w.speed_mps > 1.0f && w.gravity_scale > 0.0f) {
        // Hold over by the drop over the flight time.
        const float t = glm::length(aim_point - gh.pos) / w.speed_mps;
        aim_point.y += 0.5f * 9.81f * w.gravity_scale * t * t;
    }
    const Vec3 aim_dir = glm::normalize(aim_point - gh.pos);
    gh.rot = look_along(aim_dir);
    if (w.bow && ctx.sockets != nullptr && ctx.sockets->has_socket(w.entity, "string_rest") &&
        ctx.sockets->has_socket(w.entity, "arrow_rest") && phase_ != Phase::Pick) {
        // A bow is held by its grip, so the arrow flies along the string ->
        // rest line, not along the hand: turn the hand until that line
        // points at the target (the bow follows the hand rigidly).
        const Vec3 sr(ctx.sockets->socket_world(w.entity, "string_rest")[3]);
        const Vec3 ar(ctx.sockets->socket_world(w.entity, "arrow_rest")[3]);
        if (glm::length(ar - sr) > 1e-3f) {
            const Vec3 cur = glm::normalize(ar - sr);
            const float c = std::clamp(glm::dot(cur, aim_dir), -1.0f, 1.0f);
            Vec3 axis = glm::cross(cur, aim_dir);
            Quat delta(1.0f, 0.0f, 0.0f, 0.0f);
            if (glm::length(axis) > 1e-5f && c < 0.99999f) {
                delta = glm::angleAxis(std::acos(c), glm::normalize(axis));
            }
            bow_rot_ = glm::normalize(delta * bow_rot_);
        }
        gh.rot = bow_rot_;
    } else if (w.bow) {
        bow_rot_ = gh.rot;
    }

    // A two-handed weapon (a rifle's handguard, an RPG's rear grip) gets the
    // other hand on its support socket once the first hand has it -- some
    // do not fire one-handed at all, and it exercises the two-hand solve.
    const bool supported = !w.bow && (phase_ == Phase::Hold || phase_ == Phase::Fire);
    if (supported && world.has<CHoldProfile>(w.entity) && ctx.sockets != nullptr) {
        const auto& hp = world.get<CHoldProfile>(w.entity);
        if (hp.two_hand != TwoHandMode::None && !hp.support_socket.empty() &&
            ctx.sockets->has_socket(w.entity, hp.support_socket.c_str())) {
            auto& sh = hands_[other];
            sh.active = true;
            sh.pos = Vec3(ctx.sockets->socket_world(w.entity, hp.support_socket.c_str())[3]);
            sh.rot = gh.rot;
            sh.trigger = 0.0f;
            sh.grip = (phase_ == Phase::Fire || phase_t_ > 0.4f) ? 1.0f : 0.0f;
        }
    }

    switch (phase_) {
        case Phase::Pick: {
            if (ctx.hands != nullptr) ctx.hands->request_hold(gun_hand, w.entity);
            gh.grip = 1.0f;
            gh.trigger = 0.0f;
            seen_fire_seq_ = 0;
            WeaponState s{};
            if (ctx.weapons != nullptr && ctx.weapons->state(w.entity, &s)) seen_fire_seq_ = s.fire_seq;
            seen_hit_seq_ = ctx.weapons != nullptr ? ctx.weapons->hit_seq() : 0;
            seen_proj_seq_ = ctx.projectiles != nullptr ? ctx.projectiles->hit_seq() : 0;
            shots_ = hits_ = max_live_ = 0;
            frame_ms_.clear();
            target_health_start_ = (target_ != kNone && ctx.combat != nullptr)
                                       ? ctx.combat->health(target_) : 0.0f;
            phase_ = Phase::Hold;
            phase_t_ = 0.0f;
            return;
        }
        case Phase::Hold: {
            gh.grip = 1.0f;
            if (phase_t_ < kHoldS) return;
            const std::uint32_t held = ctx.hands != nullptr ? ctx.hands->held_entity(gun_hand) : kNone;
            PTV_INFO("weapons: hold weapon={} e={} hand={} held={} target={}", w.name, w.entity,
                     gun_hand == 0 ? "L" : "R", held == w.entity ? 1 : 0,
                     target_ != kNone ? name_of(world, target_) : std::string("none"));
            phase_ = Phase::Fire;
            phase_t_ = 0.0f;
            return;
        }
        case Phase::Fire: {
            gh.grip = 1.0f;
            WeaponState s{};
            const bool known = ctx.weapons != nullptr && ctx.weapons->state(w.entity, &s);
            float length = kSemiS + kAutoS;
            if (w.bow) {
                length = kBowS;
                // The drawing hand: on the string, squeeze, pull back, let go.
                auto& dh = hands_[other];
                dh.active = true;
                Vec3 rest = gh.pos;
                Vec3 back = -aim_dir;
                if (ctx.sockets != nullptr && ctx.sockets->has_socket(w.entity, "string_rest")) {
                    rest = Vec3(ctx.sockets->socket_world(w.entity, "string_rest")[3]);
                    if (ctx.sockets->has_socket(w.entity, "arrow_rest")) {
                        const Vec3 ar(ctx.sockets->socket_world(w.entity, "arrow_rest")[3]);
                        if (glm::length(rest - ar) > 1e-3f) back = glm::normalize(rest - ar);
                    }
                }
                const float pull = std::clamp((phase_t_ - 0.4f) / 1.0f, 0.0f, 1.0f) * 0.5f;
                dh.pos = rest + back * pull;
                dh.rot = gh.rot;
                dh.grip = (phase_t_ >= 0.2f && phase_t_ < 1.8f) ? 1.0f : 0.0f;
                dh.trigger = 0.0f;
            } else if (known && s.delivery == WeaponDelivery::Beam) {
                length = kBeamS;
                gh.trigger = 1.0f;
            } else if (phase_t_ < kSemiS) {
                // Five pulls: 0.2 s down, 0.3 s up.
                const float in = std::fmod(phase_t_, 0.5f);
                gh.trigger = in < 0.2f ? 1.0f : 0.0f;
            } else {
                gh.trigger = 1.0f;
            }
            frame_ms_.push_back(dt * 1000.0f);
            observe(world, ctx, w, aim_dir, max_live_projectiles);
            if (phase_t_ >= length) {
                gh.trigger = 0.0f;
                finish_weapon(ctx, w);
                phase_ = Phase::Release;
                phase_t_ = 0.0f;
            }
            return;
        }
        case Phase::Release: {
            gh.trigger = 0.0f;
            gh.grip = 0.0f;
            hands_[other].grip = 0.0f;
            observe(world, ctx, w, aim_dir, max_live_projectiles);
            if (phase_t_ < kReleaseS) return;
            hands_[0] = HandScript{};
            hands_[1] = HandScript{};
            ++index_;
            phase_ = Phase::Pick;
            phase_t_ = 0.0f;
            return;
        }
        default:
            return;
    }
}

void WeaponVerifier::observe(World& world, MechanicContext& ctx, const Weapon& w,
                             const Vec3& aim_dir, int max_live) {
    if (ctx.weapons == nullptr) return;
    WeaponState s{};
    if (ctx.weapons->state(w.entity, &s) && s.fire_seq > seen_fire_seq_) {
        const std::uint64_t n = s.fire_seq - seen_fire_seq_;
        seen_fire_seq_ = s.fire_seq;
        shots_ += static_cast<int>(n);
        if (s.delivery != WeaponDelivery::Drawn && ctx.sockets != nullptr &&
            ctx.sockets->has_socket(w.entity, w.muzzle.c_str())) {
            const Mat4 m = ctx.sockets->socket_world(w.entity, w.muzzle.c_str());
            const float origin_err = glm::length(s.last_origin - Vec3(m[3])) * 100.0f;
            const float dir_err = glm::degrees(std::acos(std::clamp(
                glm::dot(glm::normalize(s.last_direction), aim_dir), -1.0f, 1.0f)));
            // Beams fire every frame: log one in ten.
            if (s.delivery != WeaponDelivery::Beam || shots_ % 10 == 1) {
                PTV_INFO("weapons: shot weapon={} seq={} delivery={} origin_err_cm={:.2f} "
                         "aim_err_deg={:.1f} ammo={} reloading={}",
                         w.name, s.fire_seq, delivery_name(s.delivery), origin_err, dir_err,
                         s.infinite ? -1 : s.ammo_in_magazine, s.reloading ? 1 : 0);
            }
        } else if (s.delivery == WeaponDelivery::Drawn) {
            PTV_INFO("weapons: shot weapon={} seq={} delivery=drawn", w.name, s.fire_seq);
        }
    }
    WeaponHit hits[16];
    const int nh = ctx.weapons->hits_since(seen_hit_seq_, hits, 16);
    for (int i = 0; i < nh; ++i) {
        seen_hit_seq_ = std::max(seen_hit_seq_, hits[i].seq);
        if (hits[i].weapon != w.entity) continue;
        ++hits_;
        PTV_INFO("weapons: hit weapon={} target={} damage={:.1f} killed={} delivery={}", w.name,
                 hits[i].target != kNone ? name_of(world, hits[i].target) : std::string("world"),
                 hits[i].damage, hits[i].killed ? 1 : 0, delivery_name(hits[i].delivery));
    }
    if (ctx.projectiles != nullptr) {
        ProjectileHit ph[16];
        const int np = ctx.projectiles->hits_since(seen_proj_seq_, ph, 16);
        for (int i = 0; i < np; ++i) {
            seen_proj_seq_ = std::max(seen_proj_seq_, ph[i].seq);
            if (ph[i].weapon != w.entity) continue;
            PTV_INFO("weapons: projectile_hit weapon={} target={} damage={:.1f}", w.name,
                     ph[i].target != kNone ? name_of(world, ph[i].target) : std::string("world"),
                     ph[i].damage);
        }
        max_live_ = std::max(max_live_, ctx.projectiles->live_count());
    }
    (void)max_live;
}

void WeaponVerifier::finish_weapon(MechanicContext& ctx, const Weapon& w) {
    float p95 = 0.0f;
    float median = 0.0f;
    int dropped = 0;
    if (!frame_ms_.empty()) {
        std::vector<float> v = frame_ms_;
        std::sort(v.begin(), v.end());
        p95 = v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * 0.95f))];
        median = v[v.size() / 2];
        // A frame that took over 1.5 display periods missed a vsync; timing
        // jitter alone never does.
        for (const float ms : v) dropped += ms > median * 1.5f ? 1 : 0;
    }
    float health_now = target_health_start_;
    if (target_ != kNone && ctx.combat != nullptr) health_now = ctx.combat->health(target_);
    WeaponState s{};
    const bool known = ctx.weapons != nullptr && ctx.weapons->state(w.entity, &s);
    PTV_INFO("weapons: weapon_done weapon={} delivery={} shots={} hits={} target_health={:.1f}->"
             "{:.1f} max_live={} p95_ms={:.2f} median_ms={:.2f} dropped={} frames={} "
             "magazine={} supported={}",
             w.name, known ? delivery_name(s.delivery) : "unknown", shots_, hits_,
             target_health_start_, health_now, max_live_, p95, median, dropped,
             frame_ms_.size(), known && !s.infinite ? s.magazine_size : 0,
             ctx.hands != nullptr && ctx.hands->support_entity(w.bow ? 1 : 0) == w.entity ? 1 : 0);
}

void WeaponVerifier::patch(InputSnapshot& s) const {
    if (phase_ == Phase::Done || phase_ == Phase::Settle) return;
    for (int h = 0; h < 2; ++h) {
        const HandScript& hs = hands_[h];
        if (!hs.active) continue;
        auto& out = s.hands[h];
        out.hand_position = hs.pos;
        out.hand_rotation = hs.rot;
        out.hand_valid = true;
        out.aim_position = hs.pos;
        out.aim_rotation = hs.rot;
        out.aim_valid = true;
        out.trigger = hs.trigger;
        out.prev_trigger = hs.prev_trigger;
        out.grip = hs.grip;
        out.prev_grip = hs.prev_grip;
        out.velocity = Vec3(0.0f);
    }
}

}  // namespace ptv
