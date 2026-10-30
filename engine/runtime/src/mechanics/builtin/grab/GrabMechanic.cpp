// builtin.grab -- pick objects up with the grip button, in one or two hands.
//
// Squeeze near a grabbable object to attach it to that hand; let go to drop
// it. While held, the object's rigid body is switched to KINEMATIC and swept
// toward its hold pose every frame, so it pushes other bodies around instead
// of tunnelling through them, and it cannot be shoved out of your grip by the
// physics solver.
//
// Player scope: the per-hand loop below must run exactly once per frame, but
// the mechanic is bound to every grabbable prop in the scene (that binding is
// what MARKS a prop as grabbable). Entity scope would run the whole loop once
// per prop and grab, drop and throw N times over.
//
// Input phase: the held entity has to be decided before the frame's
// InputSnapshot is frozen, so a weapon mechanic sees "I am now held" on the
// same frame it was picked up rather than one frame later.
//
// Where the object sits in the hand comes from its hold profile
// (``CHoldProfile``) and hand sockets (socket_generation), solved by
// HoldSolver.h: a hand-frame hold for swords and tools, an aim-aligned hold
// for guns, a line between two hands for rifles and two-handed swords, a
// support hand wrapped around the shooting hand for pistols, and a forearm
// hold for strapped shields. The visible hand is anchored to the socket it
// holds (IHands::set_hand_anchor). Items from older pipelines keep the
// legacy ``grip`` socket solve (GripHold.h), and an item with no socket at
// all keeps whatever relative pose it had when squeezed.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>

#include "../BuiltinMechanicBase.h"
#include "GrabRelease.h"
#include "GripHold.h"
#include "HoldSolver.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// Asymmetric thresholds (Schmitt trigger): squeezing past 0.7 grabs, but you
// have to relax below 0.4 to let go. A single threshold makes an object
// flicker between held and dropped while the grip rests near it.
constexpr float kGrabPress = 0.7f;
constexpr float kGrabRelease = 0.4f;
// Reach. Roughly a palm's width around the hand origin.
constexpr float kGrabRadius = 0.18f;
// How close the second hand must be to an item's support socket to steady
// it rather than take it.
constexpr float kSupportRadius = 0.16f;
// A support hand pulled this much farther than the item's own two-hand span
// lets go.
constexpr float kSupportBreakSlack = 0.15f;

constexpr std::uint32_t kNone = InputSnapshot::kInvalidEntity;

/** Nearest entity tagged "grabbable" within ``max_dist`` of ``pos``. */
std::uint32_t find_grabbable(World& world, const Vec3& pos, float max_dist) {
    entt::entity best = entt::null;
    float best_d2 = max_dist * max_dist;
    auto view = world.registry().view<const CTransform, const CTag>();
    for (auto e : view) {
        const auto& tags = view.get<const CTag>(e).tags;
        bool grabbable = false;
        for (const auto& t : tags) {
            if (t == "grabbable") {
                grabbable = true;
                break;
            }
        }
        if (!grabbable) continue;
        // An equipped item with passive protection is invisible to an
        // ordinary grab, so brushing a hand past a worn gauntlet cannot rip
        // it off. Taking it off deliberately is a different action, owned by
        // builtin.equip's grab_pull path, which goes through IEquip rather
        // than through this search.
        if (world.registry().all_of<CEquipped>(e)) {
            const auto& eq = world.registry().get<CEquipped>(e);
            if (eq.wearer != InputSnapshot::kInvalidEntity &&
                eq.passive_grab_protection) {
                continue;
            }
        }
        const auto& ct = view.get<const CTransform>(e).t;
        const Vec3 dp = ct.position - pos;
        const float d2 = glm::dot(dp, dp);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = e;
        }
    }
    return best == entt::null ? kNone : static_cast<std::uint32_t>(best);
}

enum class HoldMode { Rigid, Forearm };

class GrabMechanic : public IMechanic {
public:
    std::string id() const override { return "builtin.grab"; }
    MechanicPhase phase() const override { return MechanicPhase::Input; }
    MechanicScope scope() const override { return MechanicScope::Player; }

    // Called once per bound entity even though only one instance exists.
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& p,
                   MechanicContext&) override {
        // Per-asset correction for an asset whose grip socket is not quite
        // how it should sit in the hand (XYZ Euler, degrees, in the socket's
        // frame).
        if (p.contains("hold_rotation_offset_deg") &&
            p["hold_rotation_offset_deg"].is_array() &&
            p["hold_rotation_offset_deg"].size() == 3) {
            const auto& a = p["hold_rotation_offset_deg"];
            const Vec3 deg(a[0].get<float>(), a[1].get<float>(),
                           a[2].get<float>());
            hold_offsets_[entity] = glm::normalize(glm::quat(glm::radians(deg)));
        }
        // Binding this mechanic is what makes an object grabbable. Unify on
        // the "grabbable" tag the search reads.
        auto& tag = world.registry().get_or_emplace<CTag>(
            static_cast<entt::entity>(entity));
        if (std::find(tag.tags.begin(), tag.tags.end(), "grabbable") ==
            tag.tags.end()) {
            tag.tags.push_back("grabbable");
        }
        if (p.contains("radius_m") && p["radius_m"].is_number()) {
            radius_m_ = std::max(0.01f, p.value("radius_m", kGrabRadius));
        }
        // On by default: an asset that has a grip socket wants to be held by
        // it. Set false for something with no single correct orientation --
        // a ball, a rock, a crate.
        if (p.contains("snap_to_grip") && p["snap_to_grip"].is_boolean()) {
            snap_to_grip_ = p.value("snap_to_grip", true);
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    void on_update(World& world, std::uint32_t, float dt,
                   const InputSnapshot& input, MechanicContext& ctx) override {
        if (ctx.hands == nullptr || ctx.physics == nullptr) return;

        // Scripted / item-source requests: put an entity straight into a
        // free hand.
        for (int h = 0; h < 2; ++h) {
            const std::uint32_t req = ctx.hands->take_hold_request(h);
            if (req != kNone && hold_[h].entity == kNone && world.valid(req) &&
                input.hands[h].hand_valid) {
                attach_hold(world, h, req, input.hands[h], ctx);
            }
        }

        for (int h = 0; h < 2; ++h) {
            const auto& hand = input.hands[h];
            const bool was_pressed = hand.prev_grip >= kGrabPress;
            const bool is_pressed = hand.grip >= kGrabPress;
            const bool is_released = hand.grip < kGrabRelease;

            if (!was_pressed && is_pressed && hold_[h].entity == kNone &&
                support_[h] == kNone && hand.hand_valid) {
                if (!try_support(world, h, input, ctx)) try_grab(world, h, hand, ctx);
            } else if (was_pressed && is_released) {
                if (support_[h] != kNone) {
                    end_support(h, ctx);
                } else if (hold_[h].entity != kNone) {
                    release(world, h, input, ctx);
                }
            }
        }

        for (int h = 0; h < 2; ++h) {
            if (hold_[h].entity != kNone && input.hands[h].hand_valid) {
                drive_hold(world, h, input, ctx, dt);
            }
        }
    }

private:
    struct Hold {
        std::uint32_t entity{kNone};
        HoldMode mode{HoldMode::Rigid};
        Vec3 local_offset{0.0f, 0.0f, 0.0f};
        Quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        /** Anchor the visible hand to the primary socket. */
        bool anchored{false};
    };

    // ------------------------------------------------------------------
    // Socket lookup
    // ------------------------------------------------------------------
    struct SocketView {
        Transform t;
        const SocketInfo* info{nullptr};
    };

    static bool socket(World& world, std::uint32_t e, const std::string& name,
                       SocketView* out) {
        const auto ent = static_cast<entt::entity>(e);
        if (name.empty() || !world.registry().all_of<CMesh>(ent)) return false;
        const auto& mesh = world.registry().get<CMesh>(ent);
        const auto it = mesh.sockets.find(name);
        if (it == mesh.sockets.end()) return false;
        out->t = it->second;
        out->info = nullptr;
        if (const auto* si = world.registry().try_get<CSocketInfo>(ent)) {
            const auto jt = si->sockets.find(name);
            if (jt != si->sockets.end()) out->info = &jt->second;
        }
        return true;
    }

    static float scale_of(World& world, std::uint32_t e) {
        const auto ent = static_cast<entt::entity>(e);
        return world.registry().all_of<CTransform>(ent)
                   ? world.registry().get<CTransform>(ent).t.scale
                   : 1.0f;
    }

    static const CHoldProfile* profile(World& world, std::uint32_t e) {
        return world.registry().try_get<CHoldProfile>(static_cast<entt::entity>(e));
    }

    static Quat hand_rot_for(const SocketView& s, int hand) {
        const bool flip = s.info != nullptr && s.info->mirror == "flip_z180";
        return grab::hand_socket_rotation(s.t.rotation, hand == 0, flip);
    }

    // ------------------------------------------------------------------
    // Grabbing
    // ------------------------------------------------------------------
    void try_grab(World& world, int h, const InputSnapshot::HandState& hand,
                  MechanicContext& ctx) {
        const std::uint32_t target =
            find_grabbable(world, hand.hand_position, radius_m_);
        if (target == kNone) return;
        attach_hold(world, h, target, hand, ctx);
    }

    /** Make ``target`` hand ``h``'s primary hold, choosing the solve. */
    void attach_hold(World& world, int h, std::uint32_t target,
                     const InputSnapshot::HandState& hand, MechanicContext& ctx) {
        const auto e = static_cast<entt::entity>(target);
        if (!world.registry().all_of<CTransform>(e)) return;
        const auto& ct = world.registry().get<CTransform>(e).t;

        // Taking an item from the other hand (or from a support hand).
        const int other = 1 - h;
        if (hold_[other].entity == target) {
            hold_[other] = Hold{};
            ctx.hands->set_held_entity(other, kNone);
        }
        if (support_[other] == target) end_support(other, ctx);

        Hold hs{};
        hs.entity = target;
        const Quat grip_to_aim =
            hand.aim_valid ? glm::normalize(glm::inverse(hand.hand_rotation) *
                                            hand.aim_rotation)
                           : Quat(1.0f, 0.0f, 0.0f, 0.0f);
        const auto off = hold_offsets_.find(target);
        const Quat extra = off != hold_offsets_.end()
                               ? off->second
                               : Quat(1.0f, 0.0f, 0.0f, 0.0f);
        const float scale = scale_of(world, target);
        const CHoldProfile* hp = profile(world, target);
        SocketView prim{};
        bool solved = false;
        if (snap_to_grip_ && hp != nullptr &&
            socket(world, target, hp->primary_socket, &prim)) {
            if (hp->kind == HoldKind::ForearmStrap) {
                hs.mode = HoldMode::Forearm;
                hs.anchored = true;
                solved = true;
            } else if (hp->aim_aligned && hp->has_forward) {
                grab::solve_aim_hold(prim.t.position, hp->forward, hp->up, scale,
                                     grip_to_aim, extra, &hs.local_offset,
                                     &hs.local_rotation);
                hs.anchored = true;
                solved = true;
            } else {
                grab::solve_hand_frame_hold(prim.t.position, hand_rot_for(prim, h) * extra,
                                            scale, &hs.local_offset, &hs.local_rotation);
                hs.anchored = true;
                solved = true;
            }
        }
        if (!solved && snap_to_grip_ &&
            legacy_grip_hold(world, target, grip_to_aim, extra, &hs.local_offset,
                             &hs.local_rotation)) {
            solved = true;
        }
        if (!solved) {
            // No grip socket: keep the object's pose in the hand's frame, so
            // it holds the relative orientation it had at the moment of the
            // grab.
            const Quat inv_hand = glm::inverse(hand.hand_rotation);
            hs.local_offset = inv_hand * (ct.position - hand.hand_position);
            hs.local_rotation = inv_hand * ct.rotation;
        }
        hold_[h] = hs;
        ctx.physics->set_kinematic(target, true);
        ctx.hands->set_held_entity(h, target);
        // One line per grab that a headset session can be read back from:
        // how far the palm socket lands from the grip pose, and how far a
        // gun's barrel points from the aim ray.
        if (solved && hp != nullptr && hs.mode == HoldMode::Rigid) {
            const Vec3 obj_pos = hand.hand_position + hand.hand_rotation * hs.local_offset;
            const Quat obj_rot = glm::normalize(hand.hand_rotation * hs.local_rotation);
            const float palm_err_cm =
                100.0f * glm::length(obj_pos + obj_rot * (prim.t.position * scale) -
                                     hand.hand_position);
            float aim_err_deg = -1.0f;
            if (hp->aim_aligned && hp->has_forward && hand.aim_valid) {
                const Vec3 barrel = glm::normalize(obj_rot * hp->forward);
                const Vec3 aim = glm::normalize(hand.aim_rotation * Vec3(0, 0, -1));
                aim_err_deg = glm::degrees(std::acos(std::clamp(glm::dot(barrel, aim), -1.0f, 1.0f)));
            }
            PTV_INFO("hold check: entity={} hand={} palm_err_cm={:.2f} aim_err_deg={:.1f}",
                     target, h == 0 ? "L" : "R", palm_err_cm, aim_err_deg);
        }
        PTV_INFO("Grab: hand {} -> entity {} ({})", h == 0 ? "L" : "R", target,
                 hp ? (hs.mode == HoldMode::Forearm ? "forearm"
                       : hp->aim_aligned          ? "aim"
                                                  : "hand-frame")
                    : (solved ? "legacy grip" : "relative"));
    }

    /** The second hand squeezing near an item the first hand holds steadies
     *  it instead of taking it. */
    bool try_support(World& world, int h, const InputSnapshot& input,
                     MechanicContext& ctx) {
        const int other = 1 - h;
        const std::uint32_t item = hold_[other].entity;
        if (item == kNone) return false;
        const CHoldProfile* hp = profile(world, item);
        if (hp == nullptr || hp->support_socket.empty() ||
            hp->kind == HoldKind::ForearmStrap) {
            return false;
        }
        SocketView sup{};
        if (!socket(world, item, hp->support_socket, &sup)) return false;
        const auto& t = world.registry().get<CTransform>(static_cast<entt::entity>(item)).t;
        const Vec3 world_pos = t.position + t.rotation * (sup.t.position * t.scale);
        if (glm::length(world_pos - input.hands[h].hand_position) > kSupportRadius) {
            return false;
        }
        support_[h] = item;
        ctx.hands->set_support_entity(h, item);
        if (ctx.haptics) ctx.haptics->pulse(h, 0.04f, 0.35f);
        PTV_INFO("Grab: hand {} supports entity {}", h == 0 ? "L" : "R", item);
        return true;
    }

    void end_support(int h, MechanicContext& ctx) {
        support_[h] = kNone;
        ctx.hands->set_support_entity(h, kNone);
    }

    // ------------------------------------------------------------------
    // Holding
    // ------------------------------------------------------------------
    void drive_hold(World& world, int h, const InputSnapshot& input,
                    MechanicContext& ctx, float dt) {
        Hold& hs = hold_[h];
        const auto& hand = input.hands[h];
        const std::uint32_t item = hs.entity;
        if (!world.valid(item)) {
            hold_[h] = Hold{};
            ctx.hands->set_held_entity(h, kNone);
            return;
        }
        const float scale = scale_of(world, item);
        const CHoldProfile* hp = profile(world, item);

        Vec3 target_pos = hand.hand_position + hand.hand_rotation * hs.local_offset;
        Quat target_rot = glm::normalize(hand.hand_rotation * hs.local_rotation);

        SocketView prim{};
        const bool has_prim = hp != nullptr && socket(world, item, hp->primary_socket, &prim);

        if (hs.mode == HoldMode::Forearm && has_prim) {
            SocketView fore{};
            if (socket(world, item, hp->forearm_socket, &fore)) {
                const Vec3 item_dir = fore.t.position - prim.t.position;
                const Vec3 elbow = ctx.hands->forearm_direction(h);
                const Vec3 dorsal = grab::dorsal_direction(hand.hand_rotation, h == 0);
                if (glm::length(item_dir) > 1e-5f) {
                    target_rot = grab::line_rotation(item_dir, hp->up, elbow, dorsal);
                    target_pos = grab::place_point(prim.t.position, scale, target_rot,
                                                   hand.hand_position);
                }
            }
        }

        // A second hand steadying the item.
        const int other = 1 - h;
        if (support_[other] == item && hp != nullptr) {
            SocketView sup{};
            const auto& oh = input.hands[other];
            if (!oh.hand_valid || !socket(world, item, hp->support_socket, &sup)) {
                end_support(other, ctx);
            } else if (hp->kind == HoldKind::TwoHandLine && has_prim) {
                const Vec3 item_dir = sup.t.position - prim.t.position;
                const float span = glm::length(item_dir) * scale;
                const Vec3 world_dir = oh.hand_position - hand.hand_position;
                if (glm::length(world_dir) > span * 1.5f + kSupportBreakSlack) {
                    end_support(other, ctx);
                } else if (span > 1e-4f && glm::length(world_dir) > 1e-4f) {
                    const Vec3 item_up = hp->has_forward
                                             ? hp->up
                                             : prim.t.rotation * Vec3(0.0f, -1.0f, 0.0f);
                    const Vec3 world_up = target_rot * item_up;
                    target_rot = grab::line_rotation(item_dir, item_up, world_dir,
                                                     world_up);
                    target_pos = grab::place_point(prim.t.position, scale, target_rot,
                                                   hand.hand_position);
                }
            }
            if (support_[other] == item) {
                // Draw the support hand on its socket.
                Vec3 anchor_pos = target_pos + target_rot * (sup.t.position * scale);
                const Quat anchor_rot = glm::normalize(target_rot * hand_rot_for(sup, other));
                if (sup.info != nullptr && sup.info->lateral_offset_m > 0.0f) {
                    // A wrap sits on the side of the grip opposite the
                    // shooting hand's palm: the support hand's own side.
                    const Vec3 x = target_rot * (prim.t.rotation * Vec3(1.0f, 0.0f, 0.0f));
                    anchor_pos += x * (other == 0 ? -1.0f : 1.0f) *
                                  sup.info->lateral_offset_m * scale;
                }
                ctx.hands->set_hand_anchor(other, anchor_pos, anchor_rot, 1.0f);
            }
        }

        // Sweep the held body toward its pose. Done here, in the Input
        // phase, so the target is set BEFORE the physics step integrates it
        // -- doing it after would make held objects lag a frame and jitter.
        ctx.physics->move_kinematic(item, target_pos, target_rot, dt);

        if (hs.anchored && has_prim) {
            const Vec3 anchor_pos = target_pos + target_rot * (prim.t.position * scale);
            const Quat anchor_rot = glm::normalize(target_rot * hand_rot_for(prim, h));
            ctx.hands->set_hand_anchor(h, anchor_pos, anchor_rot,
                                       hs.mode == HoldMode::Forearm ? 0.7f : 1.0f);
        }
    }

    /** Hold offsets from a legacy (``+Z forward``) ``grip`` socket, for items
     *  whose scene predates socket_generation (see GripHold.h). */
    static bool legacy_grip_hold(World& world, std::uint32_t entity,
                                 const Quat& grip_to_aim, const Quat& extra,
                                 Vec3* out_offset, Quat* out_rotation) {
        SocketView s{};
        if (!socket(world, entity, "grip", &s)) return false;
        if (s.info != nullptr && !s.info->kind.empty()) return false;
        grab::solve_grip_hold(s.t.position, s.t.rotation, scale_of(world, entity),
                              grip_to_aim, extra, out_offset, out_rotation);
        return true;
    }

    void release(World& world, int h, const InputSnapshot& input,
                 MechanicContext& ctx) {
        const std::uint32_t held = hold_[h].entity;
        const int other = 1 - h;
        hold_[h] = Hold{};
        ctx.hands->set_held_entity(h, kNone);
        // The support hand keeps the item: it becomes the holding hand.
        if (support_[other] == held && input.hands[other].hand_valid) {
            end_support(other, ctx);
            attach_hold(world, other, held, input.hands[other], ctx);
            return;
        }
        // Back to dynamic first: an impulse on a kinematic body does nothing.
        ctx.physics->set_kinematic(held, false);
        // A throw mechanic bound to this entity turns the release into an
        // impulse. With no throw bound, the object simply drops -- which is
        // exactly what a scene that binds grab without throw is asking for.
        const auto& hand = input.hands[h];
        grab::publish_release(held, hand.velocity);
        PTV_INFO("Release: hand {} drops entity {} @ {:.2f} m/s",
                 h == 0 ? "L" : "R", held, glm::length(hand.velocity));
    }

    Hold hold_[2]{};
    std::uint32_t support_[2]{kNone, kNone};
    float radius_m_{kGrabRadius};
    bool snap_to_grip_{true};
    std::unordered_map<std::uint32_t, Quat> hold_offsets_;
};

}  // namespace

namespace grab {
namespace {
ReleaseEvent& release_slot() {
    static ReleaseEvent ev;
    return ev;
}
}  // namespace

const ReleaseEvent& last_release() { return release_slot(); }

void publish_release(std::uint32_t entity, const Vec3& hand_velocity) {
    ReleaseEvent& ev = release_slot();
    ev.entity = entity;
    ev.hand_velocity = hand_velocity;
    ++ev.serial;
}

}  // namespace grab

extern "C" int ptv_force_link_builtin_grab = 1;

PTV_REGISTER_MECHANIC("builtin.grab", GrabMechanic)

}  // namespace ptv
