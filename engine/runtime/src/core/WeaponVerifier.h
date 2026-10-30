#pragma once

// The weapons pass of on-headset verification (``debug.ptv.verify_weapons``).
//
// A headset lying on a desk has no hands in it, so the pass supplies them:
// for every weapon the player can wield, it puts the weapon in a scripted
// right hand (``IHands::request_hold`` -> builtin.grab, exactly as a pickup
// would), holds that hand in front of the head aimed at a target (the
// nearest hostile, else a clone of an NPC template 8 m ahead, else a point
// 8 m ahead), and pulls the trigger: five single pulls, then three seconds
// held -- or two seconds for a beam. A bow is held in the left hand and
// drawn and released by the right. It then drops the weapon and moves on.
//
// Everything it observes goes to the log as ``weapons:`` lines that
// ``stage_quest_verify.evaluate_weapons`` reads: where each shot left
// relative to the muzzle socket, the hits ``ctx.weapons`` reports and the
// damage the target actually took, the projectile pool against its cap, and
// the p95 frame time under sustained fire. The player cannot die meanwhile
// (verify mode), and nothing here touches a weapon's own logic.

#include <cstdint>
#include <string>
#include <vector>

#include "ptv/api/InputSnapshot.h"
#include "ptv/api/MechanicContext.h"
#include "ptv/ecs/World.h"

namespace ptv {

class WeaponVerifier {
public:
    /** Called once per frame AFTER Simulate-phase mechanics ran. */
    void tick(float dt, World& world, MechanicContext& ctx, const Vec3& head_pos,
              const Quat& head_rot, int max_live_projectiles);
    /** Overwrite the scripted hands' pose, trigger and grip. Idempotent
     *  within a frame (the engine builds the snapshot twice). */
    void patch(InputSnapshot& s) const;
    bool finished() const { return phase_ == Phase::Done; }

private:
    enum class Phase { Settle, Pick, Hold, Fire, Release, Done };
    struct Weapon {
        std::uint32_t entity{0xFFFFFFFFu};
        std::string name;
        bool bow{false};
        std::string muzzle{"muzzle"};
        /** Launch speed and gravity of what it fires (0 = a ray): the aim
         *  is raised by the drop so an arrow or rocket arrives. */
        float speed_mps{0.0f};
        float gravity_scale{0.0f};
    };
    struct HandScript {
        bool active{false};
        Vec3 pos{0.0f};
        Quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        float trigger{0.0f};
        float grip{0.0f};
        float prev_trigger{0.0f};
        float prev_grip{0.0f};
    };

    void collect(World& world);
    std::uint32_t find_target(World& world, MechanicContext& ctx, const Vec3& head_pos,
                              const Vec3& fwd);
    void observe(World& world, MechanicContext& ctx, const Weapon& w, const Vec3& aim_dir,
                 int max_live);
    void finish_weapon(MechanicContext& ctx, const Weapon& w);

    Phase phase_{Phase::Settle};
    float t_{0.0f};
    float phase_t_{0.0f};
    std::vector<Weapon> weapons_;
    std::size_t index_{0};
    std::uint32_t target_{0xFFFFFFFFu};
    std::uint32_t spawned_{0xFFFFFFFFu};
    float target_health_start_{0.0f};
    std::uint64_t seen_fire_seq_{0};
    std::uint64_t seen_hit_seq_{0};
    std::uint64_t seen_proj_seq_{0};
    int shots_{0};
    int hits_{0};
    int max_live_{0};
    std::vector<float> frame_ms_;
    HandScript hands_[2];
    Quat bow_rot_{1.0f, 0.0f, 0.0f, 0.0f};
};

}  // namespace ptv
