#pragma once

// ---------------------------------------------------------------------------
// LocomotionClipSelector -- turn "how fast is this body actually moving" into
// "which locomotion clip, played at what rate".
//
// Extracted from builtin.character_locomotion because builtin.avatar_body
// needs exactly the same answer for the player's own legs. Two copies of this
// logic would drift, and the failure mode is not subtle: the whole point is
// that the stride matches the ground covered, so an NPC and the player
// walking side by side at the same speed must scale their clips identically
// or one of them visibly skates.
//
// The hard-won part is step 4 of the original comment. Tripo retargets with
// ``animate_in_place=True``, so a walk clip carries no root motion: the feet
// cycle at whatever rate the animator authored while the engine slides the
// body at whatever speed gameplay asked for. When those disagree the feet
// skate. The industry fix is to scale playback by
// ``achieved_speed / apparent_clip_speed`` and keep that scale inside roughly
// +/-25%; past that the character moon-walks or scurries, so the right move
// is to change tier (walk -> run) rather than stretch one clip further.
// Hence the clamp plus the hysteresis that stops a body hovering on the
// boundary from flickering between two clips every frame.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <string>

#include "ptv/api/MechanicContext.h"
#include "ptv/ecs/World.h"

namespace ptv::locomotion_clips {

/** Below this a body is standing still, whatever was requested. */
constexpr float kIdleSpeedEpsilon = 0.05f;

/** How far past a tier boundary the speed must go before the clip changes,
 *  as a fraction of the boundary. */
constexpr float kTierHysteresis = 0.12f;

/** The usable window for playback-rate scaling. Outside it the motion reads
 *  as wrong rather than fast, so the tier changes instead. */
constexpr float kMinClipSpeed = 0.80f;
constexpr float kMaxClipSpeed = 1.25f;

/** Apparent ground speed of a locomotion clip, as a multiple of the body's
 *  height. A 1.8 m human walks at about 1.4 m/s and runs at about 4 m/s,
 *  which is where these come from; scaling by height keeps a house-sized
 *  creature and a knee-high one both looking right. */
constexpr float kWalkSpeedPerMeterOfHeight = 0.78f;
constexpr float kRunSpeedPerMeterOfHeight = 2.2f;

/** Cross-fade between gait changes: short enough to feel responsive, long
 *  enough to hide the pop. */
constexpr float kGaitFade = 0.18f;

enum class Gait : int { Idle = 0, Walk = 1, Run = 2 };

inline const char* gait_intent(Gait g) {
    switch (g) {
        case Gait::Walk: return "walk";
        case Gait::Run: return "run";
        case Gait::Idle:
        default: return "idle";
    }
}

/** Best available estimate of a body's height, in metres. */
inline float body_height(const World& world, std::uint32_t entity) {
    if (world.has<CMesh>(entity)) {
        const auto& m = world.get<CMesh>(entity);
        if (m.target_scale_meters > 1.0e-4f) return m.target_scale_meters;
        if (m.bounds_radius_m > 1.0e-4f) return 2.0f * m.bounds_radius_m;
    }
    if (world.has<CTransform>(entity)) {
        return std::max(0.2f, world.get<CTransform>(entity).t.scale);
    }
    return 1.8f;
}

/** Apparent speed of one locomotion clip, in m/s.
 *
 *  Prefers what the scene compiler measured into ``CMesh::animation_speeds``,
 *  falls back to a height-derived estimate. Never returns zero: it is used as
 *  a divisor. */
inline float reference_speed(const World& world, std::uint32_t entity,
                             const char* intent, float height,
                             float fallback) {
    if (world.has<CMesh>(entity)) {
        const auto& speeds = world.get<CMesh>(entity).animation_speeds;
        const auto it = speeds.find(intent);
        if (it != speeds.end() && it->second > 1.0e-3f) return it->second;
    }
    return std::max(0.1f, fallback * height);
}

/** Per-body tuning. Defaults match a 1.8 m human. */
struct Tunables {
    float walk_speed_mps{1.4f};
    float run_speed_mps{4.0f};
    /** 0 keeps the height-derived reference; positive overrides it for an
     *  asset whose clips were authored at an unusual stride. */
    float reference_walk_mps{0.0f};
    float reference_run_mps{0.0f};
};

/** Stateful because the hysteresis needs to know the tier it is leaving.
 *
 *  One instance per animated body. Call ``update`` once per frame with the
 *  speed the body ACHIEVED -- not the speed it was asked for, or a character
 *  pressed against a wall runs on the spot. */
class Selector {
public:
    Gait gait() const { return gait_; }

    /** Choose the tier, fall back to a clip the asset actually has, start it
     *  and set its playback rate. Returns the gait finally used.
     *
     *  ``variant`` picks a gait VARIANT clip ("run.punching") for the same
     *  tier when the asset has one: the body still travels at the walk/run
     *  speed the behaviour asked for, only the clip changes. Empty, or a
     *  variant the asset lacks, plays the plain tier clip.
     *
     *  A one-shot clip that is still playing (attack, hurt, die) is left
     *  alone until it finishes, rather than being replaced by the gait clip
     *  on the very next frame. */
    Gait update(const World& world, std::uint32_t entity, float achieved_speed,
                bool sprint, float height, const Tunables& tune,
                MechanicContext& ctx, const std::string& variant = {}) {
        if (ctx.skeleton == nullptr || !ctx.skeleton->has_skeleton(entity)) {
            return gait_;
        }
        if (ctx.skeleton->one_shot_playing(entity)) return gait_;

        const std::string walk_key = clip_key(ctx, entity, "walk", variant);
        const std::string run_key = clip_key(ctx, entity, "run", variant);
        const float ref_walk =
            tune.reference_walk_mps > 0.0f
                ? tune.reference_walk_mps
                : reference_speed(world, entity, walk_key.c_str(), height,
                                  kWalkSpeedPerMeterOfHeight);
        const float ref_run =
            tune.reference_run_mps > 0.0f
                ? tune.reference_run_mps
                : reference_speed(world, entity, run_key.c_str(), height,
                                  kRunSpeedPerMeterOfHeight);

        const Gait wanted =
            choose_gait(achieved_speed, sprint, ref_walk, tune);

        // Fall back down the tiers the asset actually has. A creature rig has
        // one locomotion preset, so "run" resolves to the walk clip played
        // faster rather than to nothing at all.
        Gait gait = wanted;
        if (gait == Gait::Run && !ctx.skeleton->has_clip(entity, run_key.c_str())) {
            gait = Gait::Walk;
        }
        if (gait == Gait::Walk && !ctx.skeleton->has_clip(entity, walk_key.c_str())) {
            // ...and the other way: a rig retargeted with idle + run only
            // (a charging enemy) plays its run, slowed to the clamp, at walk
            // pace. Idle there is a statue gliding toward you.
            gait = ctx.skeleton->has_clip(entity, run_key.c_str()) ? Gait::Run
                                                                    : Gait::Idle;
        }

        // play_clip is a no-op when this clip is already running, so calling
        // it every frame costs a string compare and does not restart the
        // cycle.
        if (gait == Gait::Idle && !ctx.skeleton->has_clip(entity, "idle")) {
            // No idle clip: settle into the rest pose instead of leaving the
            // walk cycle running in place.
            const char* playing = ctx.skeleton->current_clip(entity);
            if (playing != nullptr && playing[0] != '\0') {
                ctx.skeleton->fade_to_rest(entity, kGaitFade);
            }
            gait_ = Gait::Idle;
            return gait_;
        }
        const char* key = gait == Gait::Walk ? walk_key.c_str()
                          : gait == Gait::Run ? run_key.c_str()
                                              : "idle";
        ctx.skeleton->play_clip(entity, key, /*looping=*/true, kGaitFade);

        float rate = 1.0f;
        if (gait == Gait::Walk) {
            rate = achieved_speed / std::max(0.1f, ref_walk);
        } else if (gait == Gait::Run) {
            rate = achieved_speed / std::max(0.1f, ref_run);
        }
        ctx.skeleton->set_clip_speed(
            entity, std::clamp(rate, kMinClipSpeed, kMaxClipSpeed));

        gait_ = gait;
        return gait_;
    }

private:
    /** "run.punching" when the asset has that variant clip, else "run". */
    static std::string clip_key(MechanicContext& ctx, std::uint32_t entity,
                                const char* tier, const std::string& variant) {
        if (!variant.empty()) {
            std::string key = std::string(tier) + "." + variant;
            if (ctx.skeleton->has_clip(entity, key.c_str())) return key;
        }
        return tier;
    }

    /** Tier selection with hysteresis around each boundary. */
    Gait choose_gait(float speed, bool sprint, float ref_walk,
                     const Tunables& tune) const {
        // The walk/run boundary sits where a walk clip would have to stretch
        // past its usable range anyway.
        const float run_boundary =
            std::max(ref_walk * kMaxClipSpeed,
                     0.5f * (tune.walk_speed_mps + tune.run_speed_mps));
        const float up = 1.0f + kTierHysteresis;
        const float down = 1.0f - kTierHysteresis;

        if (speed <= kIdleSpeedEpsilon) return Gait::Idle;
        if (sprint) return Gait::Run;

        switch (gait_) {
            case Gait::Idle:
                return speed > kIdleSpeedEpsilon * up ? Gait::Walk
                                                      : Gait::Idle;
            case Gait::Walk:
                if (speed > run_boundary * up) return Gait::Run;
                return speed < kIdleSpeedEpsilon * down ? Gait::Idle
                                                        : Gait::Walk;
            case Gait::Run:
            default:
                return speed < run_boundary * down ? Gait::Walk : Gait::Run;
        }
    }

    Gait gait_{Gait::Idle};
};

}  // namespace ptv::locomotion_clips
