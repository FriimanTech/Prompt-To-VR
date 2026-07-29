#include "ptv/core/QualityGovernor.h"

#include <algorithm>
#include <cmath>

#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// A frame counts as "bad" once it overruns the budget by this much. VR frame
// times are naturally spiky (input, physics, a streamed upload), so reacting
// to a dead-on overshoot would make the governor twitchy.
constexpr float kBadFrameSlack = 1.25f;
// ...and "good" only when comfortably inside budget, so we do not immediately
// hand quality back and drop out of target again.
constexpr float kGoodFrameSlack = 0.80f;

// Escalation ladder. Each step is one increment of one knob; the governor
// walks up the ladder while frames keep missing and back down when they don't.
constexpr float kLodBiasStep = 0.5f;
constexpr float kCullShrinkFactor = 0.8f;

}  // namespace

void QualityGovernor::configure(const ScenePerformance& perf) {
    enabled_ = perf.governor_enabled;
    base_cull_distance_m_ = perf.cull_distance_m;
    base_foveation_level_ = perf.foveation_level;

    max_lod_bias_ = perf.governor_max_lod_bias;
    min_cull_distance_m_ = perf.governor_min_cull_distance_m;
    max_foveation_level_ =
        std::max(perf.foveation_level, perf.governor_max_foveation_level);

    const int fps = perf.target_fps > 0 ? perf.target_fps : 72;
    frame_budget_s_ = 1.0f / static_cast<float>(fps);
    escalate_after_ = std::max(1, perf.governor_escalate_after_bad_frames);
    relax_after_ = std::max(1, perf.governor_relax_after_good_frames);

    // How many discrete steps exist on the ladder: LOD bias increments, then
    // cull-distance shrinks, then foveation increments.
    const int lod_steps =
        static_cast<int>(std::ceil(max_lod_bias_ / kLodBiasStep));
    int cull_steps = 0;
    if (base_cull_distance_m_ > min_cull_distance_m_) {
        float d = base_cull_distance_m_;
        while (d > min_cull_distance_m_ && cull_steps < 12) {
            d *= kCullShrinkFactor;
            ++cull_steps;
        }
    }
    const int fov_steps =
        std::max(0, max_foveation_level_ - base_foveation_level_);
    max_pressure_ = lod_steps + cull_steps + fov_steps;

    bad_frames_ = good_frames_ = 0;
    pressure_ = 0;
    apply_pressure(0);
}

bool QualityGovernor::apply_pressure(int pressure) {
    pressure = std::max(0, std::min(pressure, max_pressure_));

    State next{};
    next.lod_bias = 0.0f;
    next.cull_distance_m = base_cull_distance_m_;
    next.foveation_level = base_foveation_level_;

    int remaining = pressure;

    // 1) LOD bias -- coarser meshes at distance. Cheapest perceptually.
    const int lod_steps =
        static_cast<int>(std::ceil(max_lod_bias_ / kLodBiasStep));
    const int used_lod = std::min(remaining, lod_steps);
    next.lod_bias = std::min(max_lod_bias_,
                             static_cast<float>(used_lod) * kLodBiasStep);
    remaining -= used_lod;

    // 2) Cull distance -- stop drawing far geometry entirely.
    while (remaining > 0 && next.cull_distance_m > min_cull_distance_m_) {
        next.cull_distance_m =
            std::max(min_cull_distance_m_,
                     next.cull_distance_m * kCullShrinkFactor);
        --remaining;
    }

    // 3) Foveation -- lower peripheral resolution. Last because it is the most
    //    visible change if the player looks around quickly.
    if (remaining > 0) {
        next.foveation_level =
            std::min(max_foveation_level_, base_foveation_level_ + remaining);
    }

    const bool changed = next.lod_bias != state_.lod_bias ||
                         next.cull_distance_m != state_.cull_distance_m ||
                         next.foveation_level != state_.foveation_level;
    state_ = next;
    pressure_ = pressure;
    return changed;
}

bool QualityGovernor::update(float frame_seconds) {
    if (!enabled_ || frame_seconds <= 0.0f) return false;

    if (frame_seconds > frame_budget_s_ * kBadFrameSlack) {
        ++bad_frames_;
        good_frames_ = 0;
    } else if (frame_seconds < frame_budget_s_ * kGoodFrameSlack) {
        ++good_frames_;
        bad_frames_ = 0;
    } else {
        // In the dead band: hold steady rather than drifting either way.
        return false;
    }

    if (bad_frames_ >= escalate_after_ && pressure_ < max_pressure_) {
        bad_frames_ = 0;
        const bool changed = apply_pressure(pressure_ + 1);
        if (changed) {
            PTV_INFO(
                "governor: frames slow -> quality step {} (lod_bias={:.1f} "
                "cull={:.0f}m ffr={})",
                pressure_, state_.lod_bias, state_.cull_distance_m,
                state_.foveation_level);
        }
        return changed;
    }

    if (good_frames_ >= relax_after_ && pressure_ > 0) {
        good_frames_ = 0;
        const bool changed = apply_pressure(pressure_ - 1);
        if (changed) {
            PTV_INFO(
                "governor: headroom -> quality step {} (lod_bias={:.1f} "
                "cull={:.0f}m ffr={})",
                pressure_, state_.lod_bias, state_.cull_distance_m,
                state_.foveation_level);
        }
        return changed;
    }
    return false;
}

}  // namespace ptv
