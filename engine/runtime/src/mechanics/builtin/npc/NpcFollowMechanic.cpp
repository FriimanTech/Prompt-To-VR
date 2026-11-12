// builtin.npc.follow -- a companion that stays with its leader.
//
// The most common NPC request after "an enemy that attacks me" is "a friend
// who follows me around". It needs a trailing band (not underfoot, not left
// behind), routing around walls, a walk that becomes a run when it falls
// behind, and -- the part every hand-written version forgot -- catching up
// when it is lost: stuck behind geometry, or left behind by a teleport. A
// companion that is simply nowhere to be found is the bug this exists to
// prevent.
//
// It writes a CLocomotionIntent and nothing else; builtin.character_locomotion
// (bound alongside) moves the body and plays the gait clips.
//
// Params:
//   leader_tag        "player" (default) or any tag; the player is found
//                     through ctx.hands first.
//   min_distance_m    stop when this close (default 1.8)
//   max_distance_m    start walking when farther (default 2.8)
//   run_distance_m    run when farther (default 6.0)
//   walk_speed_mps    (1.4)   run_speed_mps (3.5)
//   catchup_distance_m  re-place behind the leader when farther (18)
//   stuck_catchup_s   ...or when it has made no progress for this long (4)
//   catch_up          false disables re-placing entirely (default true)

#include <algorithm>
#include <cmath>
#include <string>

#include "../BuiltinMechanicBase.h"
#include "ptv/core/Logger.h"

namespace ptv {
namespace {

constexpr std::uint32_t kInvalid = 0xFFFFFFFFu;

class NpcFollowMechanic final : public BuiltinMechanicBase<NpcFollowMechanic> {
public:
    static constexpr const char* kId = "builtin.npc.follow";

    void configure(const nlohmann::json& p) {
        leader_tag_ = p.value("leader_tag", std::string("player"));
        min_d_ = std::clamp(p.value("min_distance_m", 1.8f), 0.3f, 50.0f);
        max_d_ = std::clamp(p.value("max_distance_m", 2.8f), min_d_ + 0.1f, 80.0f);
        run_d_ = std::clamp(p.value("run_distance_m", 6.0f), max_d_, 200.0f);
        walk_ = std::clamp(p.value("walk_speed_mps", 1.4f), 0.1f, 20.0f);
        run_ = std::clamp(p.value("run_speed_mps", 3.5f), walk_, 40.0f);
        catchup_d_ = std::clamp(p.value("catchup_distance_m", 18.0f), run_d_, 1000.0f);
        stuck_s_ = std::clamp(p.value("stuck_catchup_s", 4.0f), 0.5f, 120.0f);
        catch_up_ = p.value("catch_up", true);
    }

    void tick(World& world, std::uint32_t self, float dt, const InputSnapshot& input,
              MechanicContext& ctx) {
        if (!world.valid(self) || !world.has<CTransform>(self)) return;
        if (world.has<CHealth>(self) && !world.get<CHealth>(self).alive) return;
        const std::uint32_t leader = find_leader(world, ctx, self);
        auto& want = world.registry().get_or_emplace<CLocomotionIntent>(
            static_cast<entt::entity>(self));
        want.face_travel = true;
        want.has_face_target = false;
        if (leader == kInvalid) {
            want.desired_velocity = Vec3(0.0f);
            return;
        }

        auto& tr = world.get<CTransform>(self).t;
        const Vec3 me = tr.position;
        const Vec3 them = leader_position(world, ctx, leader);
        Vec3 flat = them - me;
        flat.y = 0.0f;
        const float d = glm::length(flat);
        const float moved = has_last_ ? glm::length(Vec3(me.x - last_.x, 0.0f, me.z - last_.z))
                                      : 0.0f;
        last_ = me;
        has_last_ = true;

        // Lost: too far, or held in place while it should be closing in. A
        // follower walking freely behind a leader who walks away is
        // trailing, not lost (catchup_distance_m handles a big gap).
        if (d > max_d_) {
            if (d < best_d_ - 0.25f) {
                best_d_ = d;
                stuck_t_ = 0.0f;
            } else if (!moving_ || moved < 0.25f * walk_ * dt) {
                stuck_t_ += dt;
            } else {
                stuck_t_ = std::max(0.0f, stuck_t_ - dt);
            }
        } else {
            best_d_ = d;
            stuck_t_ = 0.0f;
        }
        if (catch_up_ && (d > catchup_d_ || stuck_t_ > stuck_s_)) {
            place_behind(world, ctx, input, self, them);
            want.desired_velocity = Vec3(0.0f);
            stuck_t_ = 0.0f;
            best_d_ = 1e9f;
            return;
        }

        // Hysteresis: start walking past max, stop inside min.
        if (moving_ ? d < min_d_ : d < max_d_) {
            moving_ = false;
            want.desired_velocity = Vec3(0.0f);
            want.sprint = false;
            return;
        }
        moving_ = true;
        Vec3 target = them;
        if (ctx.nav != nullptr) target = ctx.nav->next_waypoint(self, them);
        Vec3 dir = target - me;
        dir.y = 0.0f;
        if (glm::length(dir) < 1e-4f) dir = flat;
        if (glm::length(dir) < 1e-4f) {
            want.desired_velocity = Vec3(0.0f);
            return;
        }
        const bool running = d > run_d_;
        want.desired_velocity = glm::normalize(dir) * (running ? run_ : walk_);
        want.sprint = running;
    }

private:
    std::uint32_t find_leader(World& world, MechanicContext& ctx,
                              std::uint32_t self) const {
        if (leader_tag_ == "player" && ctx.hands != nullptr) {
            const std::uint32_t p = ctx.hands->player_entity();
            if (p != kInvalid && world.valid(p)) return p;
        }
        auto view = world.registry().view<const CTag, const CTransform>();
        for (const auto e : view) {
            if (static_cast<std::uint32_t>(e) == self) continue;
            for (const auto& t : view.get<const CTag>(e).tags) {
                if (t == leader_tag_) return static_cast<std::uint32_t>(e);
            }
        }
        return kInvalid;
    }

    /** The leader's FEET: for the player that is where the headset is over
     *  the floor, not the stage origin. */
    Vec3 leader_position(World& world, MechanicContext& ctx,
                         std::uint32_t leader) const {
        if (ctx.hands != nullptr && leader == ctx.hands->player_entity() &&
            ctx.locomotion != nullptr) {
            return ctx.locomotion->body_position();
        }
        return world.get<CTransform>(leader).t.position;
    }

    /** Re-place behind the leader, outside their view, on walkable floor. */
    void place_behind(World& world, MechanicContext& ctx, const InputSnapshot& input,
                      std::uint32_t self, const Vec3& leader) {
        Vec3 fwd(0.0f, 0.0f, -1.0f);
        if (input.head_valid) fwd = input.head_rotation * Vec3(0.0f, 0.0f, -1.0f);
        fwd.y = 0.0f;
        if (glm::length(fwd) < 1e-3f) fwd = Vec3(0.0f, 0.0f, -1.0f);
        fwd = glm::normalize(fwd);
        const float trail = 0.5f * (min_d_ + max_d_);
        // No reachable spot behind: right behind the leader, who is on
        // walkable floor (an unchecked point beside them was off the map).
        Vec3 spot = leader - fwd * 0.8f;
        if (ctx.nav != nullptr) {
            spot = ctx.nav->project_to_ground(ctx.nav->nearest_walkable(spot));
        }
        for (int k = 0; k < 8; ++k) {
            // Fan out behind the leader until a reachable spot turns up.
            const float a = (k % 2 == 0 ? 1.0f : -1.0f) * 0.35f * static_cast<float>((k + 1) / 2);
            const Vec3 back(-fwd.x * std::cos(a) + fwd.z * std::sin(a), 0.0f,
                            -fwd.z * std::cos(a) - fwd.x * std::sin(a));
            Vec3 p = leader + back * trail;
            if (ctx.nav != nullptr) {
                // Inside the declared walkable space: "behind" a player at
                // the edge of a yard is past its fence.
                const Vec3 inside = ctx.nav->nearest_walkable(p);
                const Vec3 d(inside.x - p.x, 0.0f, inside.z - p.z);
                if (glm::length(d) > 0.5f * trail) continue;
                p = ctx.nav->project_to_ground(inside);
                if (!ctx.nav->is_reachable(leader, p)) continue;
            }
            spot = p;
            break;
        }
        auto& t = world.get<CTransform>(self).t;
        // ``spot`` is a point on the floor; the origin sits the measured foot
        // offset above the soles (it was put AT the floor, sunk by half its
        // height).
        float foot = 0.0f;
        if (world.has<CMesh>(self)) {
            const auto& m = world.get<CMesh>(self);
            foot = m.foot_offset_m > 1e-4f ? m.foot_offset_m
                                           : 0.5f * std::max(0.0f, m.target_scale_meters);
        }
        spot.y = std::max(spot.y, leader.y) + foot;
        t.position = spot;
        if (ctx.physics != nullptr && ctx.physics->has_body(self)) {
            ctx.physics->teleport_body(self, t.position, t.rotation);
        }
        PTV_INFO("builtin.npc.follow: entity {} was lost; re-placed behind "
                 "its leader",
                 self);
    }

    std::string leader_tag_{"player"};
    float min_d_{1.8f}, max_d_{2.8f}, run_d_{6.0f};
    float walk_{1.4f}, run_{3.5f};
    float catchup_d_{18.0f}, stuck_s_{4.0f};
    bool catch_up_{true};
    bool moving_{false};
    float best_d_{1e9f};
    float stuck_t_{0.0f};
    Vec3 last_{0.0f};
    bool has_last_{false};
};

}  // namespace

extern "C" int ptv_force_link_builtin_npc_follow = 1;

PTV_REGISTER_MECHANIC("builtin.npc.follow", NpcFollowMechanic)

}  // namespace ptv
