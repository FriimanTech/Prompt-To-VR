#include "ptv/core/QualityGovernor.h"

#include <algorithm>
#include <cmath>

#include "ptv/core/Logger.h"

namespace ptv {
namespace {

// A frame counts as "bad" once it overruns the budget by this much. VR frame
// times are naturally spiky (input, physics, a streamed upload), so reacting
// to a dead-on overshoot would make the governor twitchy.
constexpr float kBadFrameSlack = 1.10f;
// ...and "good" only when comfortably inside budget, so we do not immediately
// hand quality back and drop out of target again.
constexpr float kGoodFrameSlack = 0.75f;

constexpr float kLodBiasStep = 0.5f;
constexpr float kCullShrinkFactor = 0.8f;
constexpr float kScaleStep = 0.05f;
// A step that bought less than this share of the bottleneck's time is
// marked ineffective for the rest of the scene.
constexpr float kMinGain = 0.03f;
// Foveation level the first (least visible) FFR rung stops at.
constexpr int kFovLowCap = 2;

const char* knob_name(int k) {
    static const char* names[] = {"ffr", "render_scale", "ffr", "lod_bias", "cull"};
    return (k >= 0 && k < 5) ? names[k] : "?";
}

}  // namespace

void QualityGovernor::configure(const ScenePerformance& perf) {
    enabled_ = perf.governor_enabled;
    base_.lod_bias = 0.0f;
    base_.cull_distance_m = perf.cull_distance_m;
    base_.foveation_level = perf.foveation_level;
    base_.render_scale = 1.0f;

    max_lod_bias_ = perf.governor_max_lod_bias;
    min_cull_distance_m_ = perf.governor_min_cull_distance_m;
    max_foveation_level_ = std::max(perf.foveation_level, perf.governor_max_foveation_level);
    scale_floor_ = std::clamp(perf.render_scale_floor, 0.5f, 1.0f);

    // User locks: "auto" moves; anything else pins the knob.
    lock_ffr_ = perf.lock_ffr == "off" || perf.lock_ffr == "fixed";
    if (perf.lock_ffr == "max" && perf.lock_ffr_level >= 0) {
        max_foveation_level_ = std::min(max_foveation_level_, perf.lock_ffr_level);
    }
    lock_scale_ = perf.lock_resolution == "off" || perf.lock_resolution == "high";
    lock_lod_ = perf.lock_lod == "detail";
    lock_cull_ = perf.lock_draw_distance == "far";

    const int fps = perf.refresh_hz > 0 ? perf.refresh_hz
                                        : (perf.target_fps > 0 ? perf.target_fps : 72);
    frame_budget_ms_ = 1000.0f / static_cast<float>(fps);
    escalate_after_ = std::max(1, perf.governor_escalate_after_bad_frames);
    relax_after_ = std::max(1, perf.governor_relax_after_good_frames);
    cooldown_frames_ = std::max(20, fps / 2);

    state_ = base_;
    bad_frames_ = good_frames_ = 0;
    cooldown_ = 0;
    probing_ = false;
    window_gpu_ms_ = window_cpu_ms_ = 0.0f;
    window_n_ = 0;
    ineffective_.fill(false);
    history_count_ = 0;
    blocked_logged_ = false;
}

bool QualityGovernor::can_raise(Knob k) const {
    if (ineffective_[static_cast<size_t>(k)]) return false;
    switch (k) {
        case Knob::FovLow:
            return !lock_ffr_ && state_.foveation_level < std::min(kFovLowCap, max_foveation_level_);
        case Knob::FovHigh:
            return !lock_ffr_ && state_.foveation_level < max_foveation_level_;
        case Knob::Scale:
            return !lock_scale_ && state_.render_scale > scale_floor_ + 1e-3f;
        case Knob::Lod:
            return !lock_lod_ && state_.lod_bias + 1e-3f < max_lod_bias_;
        case Knob::Cull:
            return !lock_cull_ && state_.cull_distance_m > min_cull_distance_m_ + 1e-3f &&
                   state_.cull_distance_m < 50000.0f;
        default:
            return false;
    }
}

void QualityGovernor::apply(Knob k, int dir) {
    switch (k) {
        case Knob::FovLow:
        case Knob::FovHigh:
            state_.foveation_level = std::clamp(state_.foveation_level + dir, base_.foveation_level,
                                                max_foveation_level_);
            break;
        case Knob::Scale:
            state_.render_scale =
                std::clamp(state_.render_scale - kScaleStep * static_cast<float>(dir), scale_floor_,
                           1.0f);
            break;
        case Knob::Lod:
            state_.lod_bias =
                std::clamp(state_.lod_bias + kLodBiasStep * static_cast<float>(dir), 0.0f,
                           max_lod_bias_);
            break;
        case Knob::Cull:
            if (dir > 0) {
                state_.cull_distance_m =
                    std::max(min_cull_distance_m_, state_.cull_distance_m * kCullShrinkFactor);
            } else {
                state_.cull_distance_m =
                    std::min(base_.cull_distance_m, state_.cull_distance_m / kCullShrinkFactor);
            }
            break;
        default:
            break;
    }
}

void QualityGovernor::log_change(const char* why, Knob k) {
    PTV_INFO("governor: {} -> {} (step {}: ffr {} scale {:.2f} lod_bias {:.1f} cull {:.0f}m)", why,
             knob_name(static_cast<int>(k)), history_count_, state_.foveation_level,
             state_.render_scale, state_.lod_bias, state_.cull_distance_m);
}

bool QualityGovernor::step_up(bool gpu_bound) {
    static constexpr Knob kGpu[] = {Knob::FovLow, Knob::Scale, Knob::FovHigh, Knob::Lod,
                                    Knob::Cull};
    static constexpr Knob kCpu[] = {Knob::Cull, Knob::Lod};
    const Knob* ladder = gpu_bound ? kGpu : kCpu;
    const int n = gpu_bound ? 5 : 2;
    for (int i = 0; i < n; ++i) {
        const Knob k = ladder[i];
        if (!can_raise(k) || history_count_ >= history_.size()) continue;
        apply(k, +1);
        history_[history_count_++] = k;
        probing_ = true;
        probe_knob_ = k;
        probe_gpu_ = gpu_bound;
        log_change(gpu_bound ? "GPU-bound" : "CPU-bound", k);
        return true;
    }
    return false;
}

bool QualityGovernor::step_down() {
    if (history_count_ == 0) return false;
    const Knob k = history_[--history_count_];
    apply(k, -1);
    log_change("headroom", k);
    return true;
}

bool QualityGovernor::update(float frame_seconds, float gpu_ms, float cpu_ms) {
    if (!enabled_ || frame_seconds <= 0.0f) return false;
    const float frame_ms = frame_seconds * 1000.0f;
    if (gpu_ms <= 0.0f) gpu_ms = frame_ms;
    if (cpu_ms <= 0.0f) cpu_ms = 0.0f;

    window_gpu_ms_ += gpu_ms;
    window_cpu_ms_ += cpu_ms;
    ++window_n_;

    if (cooldown_ > 0) {
        --cooldown_;
        if (cooldown_ == 0) {
            window_gpu_ms_ = window_cpu_ms_ = 0.0f;
            window_n_ = 0;
        }
        return false;
    }

    // Probe the last escalation once a settled window has been measured.
    if (probing_ && window_n_ >= escalate_after_) {
        const float after = (probe_gpu_ ? window_gpu_ms_ : window_cpu_ms_) /
                            static_cast<float>(window_n_);
        if (probe_before_ms_ > 0.0f && after > probe_before_ms_ * (1.0f - kMinGain)) {
            ineffective_[static_cast<size_t>(probe_knob_)] = true;
            PTV_INFO("governor: {} step bought nothing ({:.2f} -> {:.2f} ms); skipping it",
                     knob_name(static_cast<int>(probe_knob_)), probe_before_ms_, after);
        }
        probing_ = false;
    }

    const float worst = std::max(gpu_ms, cpu_ms);
    if (frame_ms > frame_budget_ms_ * kBadFrameSlack || worst > frame_budget_ms_ * kBadFrameSlack) {
        ++bad_frames_;
        good_frames_ = 0;
    } else if (frame_ms < frame_budget_ms_ * kGoodFrameSlack && worst < frame_budget_ms_ * kGoodFrameSlack) {
        ++good_frames_;
        bad_frames_ = 0;
    } else {
        return false;
    }

    if (bad_frames_ >= escalate_after_) {
        bad_frames_ = 0;
        const float avg_gpu = window_n_ ? window_gpu_ms_ / static_cast<float>(window_n_) : gpu_ms;
        const float avg_cpu = window_n_ ? window_cpu_ms_ / static_cast<float>(window_n_) : cpu_ms;
        const bool gpu_bound = avg_gpu >= avg_cpu;
        probe_before_ms_ = gpu_bound ? avg_gpu : avg_cpu;
        bool changed = step_up(gpu_bound);
        if (!changed) changed = step_up(!gpu_bound);
        if (changed) {
            cooldown_ = cooldown_frames_;
            blocked_logged_ = false;
            return true;
        }
        if (!blocked_logged_) {
            const bool any_lock = lock_ffr_ || lock_scale_ || lock_lod_ || lock_cull_;
            if (any_lock) {
                PTV_WARN("governor: over budget, blocked by user settings ({} ms GPU, {} ms CPU "
                         "of {:.1f} ms)", static_cast<int>(avg_gpu), static_cast<int>(avg_cpu),
                         frame_budget_ms_);
            } else {
                PTV_WARN("governor: over budget with every quality step already taken");
            }
            blocked_logged_ = true;
        }
        return false;
    }

    if (good_frames_ >= relax_after_) {
        good_frames_ = 0;
        if (step_down()) {
            probing_ = false;
            cooldown_ = cooldown_frames_;
            return true;
        }
    }
    return false;
}

}  // namespace ptv
