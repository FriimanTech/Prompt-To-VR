#pragma once

// The interactions pass of on-headset verification
// (``debug.ptv.verify_interactions``).
//
// The shooting range shipped with props the player's invisible body shoved
// aside, a red button that never pressed and grenades that never exploded --
// none of it visible to a check that only watched NPCs walk. This pass stands
// a scripted player next to every interactive thing in the scene and uses it
// the way a person would, with the same input path a real hand drives:
//
//   * settle   -- every loose prop, untouched for three seconds, must stay put;
//   * walk     -- the player walks into and past each grabbable;
//   * grab     -- a scripted right hand reaches for each grabbable with the
//                 grip open (nothing may be knocked away), closes, lifts,
//                 lets go;
//   * press    -- an index-finger poke through each button's cap, else the
//                 trigger beside it;
//   * challenge-- a button that starts a target challenge must start it:
//                 state, spawned targets at the world's anchors, the clock;
//   * throw    -- each explosive is picked up, armed with the trigger and
//                 thrown; it must detonate;
//   * views    -- the player stands at each scene_review viewpoint facing its
//                 target, for a Quest screenshot.
//
// Everything goes to the log as ``interact:`` lines that
// ``stage_quest_verify.evaluate_interactions`` reads. The player cannot die
// meanwhile (verify mode); nothing here changes a mechanic's own logic.

#include <cstdint>
#include <string>
#include <vector>

#include "ptv/api/InputSnapshot.h"
#include "ptv/api/MechanicContext.h"
#include "ptv/ecs/World.h"
#include "ptv/scene/Scene.h"

namespace ptv {

class InteractionVerifier {
public:
    explicit InteractionVerifier(std::vector<VerifyViewpoint> viewpoints)
        : viewpoints_(std::move(viewpoints)) {}
    /** Called once per frame AFTER Simulate-phase mechanics ran. */
    void tick(float dt, World& world, MechanicContext& ctx, const Vec3& head_pos,
              const Quat& head_rot);
    /** The scripted world pose of hand ``h``, when the pass is driving it.
     *  The engine uses it IN PLACE of the tracked pose, so the drawn hand,
     *  its fingertip, its colliders, its velocity and the snapshot all agree
     *  -- a poke is a fingertip crossing a cap, not a number in an input. */
    bool scripted_pose(int h, Vec3* pos, Quat* rot, bool* valid) const;
    /** Overwrite the scripted hands' trigger and grip. */
    void patch(InputSnapshot& s) const;
    bool finished() const { return phase_ == Phase::Done; }
    /** The scripted player's eyes above its feet. Until the viewpoint
     *  screenshots (which need the REAL headset's view) the engine replaces
     *  the head pose with a standing, level one at this height: a headset on
     *  a desk measured 1.18 m, the full-body avatar shrank to 0.75 and faced
     *  wherever the desk pointed, and its arms could not reach the scripted
     *  hand, so no fingertip ever met a button. */
    static constexpr float kEyeHeightM = 1.6f;
    bool scripts_head() const { return phase_ != Phase::Done && phase_ != Phase::Views; }

private:
    enum class Phase { Settle, Walk, Grab, Press, Challenge, Throw, Views, Done };
    struct Item {
        std::uint32_t entity{0xFFFFFFFFu};
        std::string name;
        Vec3 start{0.0f};
        /** Its pose at the start, to put it back after it was picked up. */
        Vec3 home_pos{0.0f};
        Quat home_rot{1.0f, 0.0f, 0.0f, 0.0f};
        float height_above_floor{0.0f};
        std::string signal;      // buttons
        std::string challenge;   // challenges: id
    };
    struct HandScript {
        bool active{false};
        bool valid{true};
        Vec3 pos{0.0f};
        Quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        float trigger{0.0f};
        float grip{0.0f};
        float prev_trigger{0.0f};
        float prev_grip{0.0f};
    };

    void collect(World& world, MechanicContext& ctx);
    void next(Phase p);
    Vec3 where(World& world, MechanicContext& ctx, std::uint32_t e) const;
    float floor_below(MechanicContext& ctx, std::uint32_t self, const Vec3& p) const;
    void stand_facing(World& world, MechanicContext& ctx, const Vec3& target, float dist);
    void tick_walk(float dt, World& world, MechanicContext& ctx);
    void tick_grab(World& world, MechanicContext& ctx, const Vec3& head);
    void tick_press(World& world, MechanicContext& ctx, const Vec3& head);
    void tick_challenge(World& world, MechanicContext& ctx);
    void tick_throw(World& world, MechanicContext& ctx, const Vec3& head);
    void tick_views(World& world, MechanicContext& ctx, const Quat& head_rot);

    Phase phase_{Phase::Settle};
    float t_{0.0f};
    float phase_t_{0.0f};
    std::size_t index_{0};
    bool collected_{false};
    std::vector<Item> props_;
    std::vector<Item> buttons_;
    std::vector<Item> challenges_;
    std::vector<Item> explosives_;
    std::vector<VerifyViewpoint> viewpoints_;
    std::vector<std::string> pressed_signals_;
    Vec3 mark_{0.0f};
    float pushed_{0.0f};
    bool flag_{false};
    bool flag_restored_{false};
    // Viewpoints: where the rig was put, and whether the eye error was fixed.
    Vec3 real_head_{0.0f};
    Vec3 view_feet_{0.0f};
    float view_yaw_{0.0f};
    bool view_fixed_{false};
    bool self_view_done_{false};
    std::uint32_t count0_{0};
    float var0_{0.0f};
    Vec3 tip_offset_{0.0f};
    bool have_tip_offset_{false};
    std::string how_;
    Vec3 dir_{0.0f, 0.0f, -1.0f};
    Vec3 hand_from_{0.0f};
    float fuse_s_{3.0f};
    HandScript hands_[2]{};
};

}  // namespace ptv
