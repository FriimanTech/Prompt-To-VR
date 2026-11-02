#pragma once

#include <array>

#include "ptv/scene/Scene.h"

namespace ptv {

/**
 * Runtime adaptive quality controller.
 *
 * The build-time budget (``ScenePerformance``) is only an ESTIMATE of what a
 * scene costs. The governor closes the loop on the device: it watches the
 * measured GPU and CPU time of each frame and trades the least visible
 * quality for frame rate until the headset holds its refresh, then gives the
 * quality back when there is headroom.
 *
 * It first decides WHICH processor is the bottleneck, because the remedies
 * differ: resolution and foveation only help a GPU-bound frame, while a
 * CPU-bound frame needs fewer objects to cull, sort and record.
 *
 *   GPU ladder (least visible first):
 *     1. foveation +1 up to level 2   (periphery only)
 *     2. render scale -0.05 down to the floor (whole image, slightly softer)
 *     3. foveation up to the governor maximum
 *     4. LOD bias +0.5 up to the maximum (distant meshes coarser sooner)
 *     5. cull distance x0.8 down to the minimum
 *   CPU ladder:
 *     1. cull distance x0.8 down to the minimum (fewer objects to record)
 *     2. LOD bias +0.5 (fewer triangles to stream and upload)
 *
 * Every step is followed by a COOLDOWN (frames ignored while the change
 * settles) and a PROBE: if the next window shows the step bought less than
 * a few percent of the bottleneck's time, the knob is marked ineffective for
 * this scene and skipped from then on -- so a scene that is not fill-bound
 * never pays for foveation that does nothing. Relaxation walks back in
 * reverse, more slowly.
 *
 * User locks ("Custom Quest Optimizations") are absolute: a locked knob is
 * never moved, whatever the frame rate does.
 */
class QualityGovernor {
public:
    struct State {
        float lod_bias{0.0f};
        float cull_distance_m{100000.0f};
        int foveation_level{0};
        float render_scale{1.0f};
    };

    /** Adopt a scene's budget and locks. Resets to that scene's baseline. */
    void configure(const ScenePerformance& perf);

    /** Feed one frame. ``gpu_ms`` / ``cpu_ms`` <= 0 means unknown, in which
     *  case the whole frame time is treated as GPU-bound.
     *  @return true when the state changed and must be pushed to the
     *          renderer / XR session. */
    bool update(float frame_seconds, float gpu_ms, float cpu_ms);
    bool update(float frame_seconds) { return update(frame_seconds, 0.0f, 0.0f); }

    const State& state() const { return state_; }
    bool enabled() const { return enabled_; }
    /** Steps taken from full quality (0 = full quality). */
    int pressure() const { return static_cast<int>(history_count_); }

private:
    enum class Knob { FovLow, Scale, FovHigh, Lod, Cull, Count };
    bool step_up(bool gpu_bound);
    bool step_down();
    bool can_raise(Knob k) const;
    void apply(Knob k, int dir);
    void log_change(const char* why, Knob k);

    bool enabled_{false};
    State state_{};
    State base_{};

    float max_lod_bias_{2.0f};
    float min_cull_distance_m_{25.0f};
    int max_foveation_level_{3};
    float scale_floor_{0.85f};
    bool lock_ffr_{false};
    bool lock_scale_{false};
    bool lock_lod_{false};
    bool lock_cull_{false};

    float frame_budget_ms_{1000.0f / 72.0f};
    int escalate_after_{30};
    int relax_after_{180};
    int cooldown_frames_{45};

    int bad_frames_{0};
    int good_frames_{0};
    int cooldown_{0};
    bool blocked_logged_{false};

    // Probe: the bottleneck's time just before the last escalation, and the
    // knob it moved; evaluated once the cooldown has passed.
    bool probing_{false};
    Knob probe_knob_{Knob::Count};
    bool probe_gpu_{true};
    float probe_before_ms_{0.0f};
    float window_gpu_ms_{0.0f};
    float window_cpu_ms_{0.0f};
    int window_n_{0};
    std::array<bool, static_cast<size_t>(Knob::Count)> ineffective_{};

    // Escalations in order, so relaxation undoes exactly what was done.
    std::array<Knob, 64> history_{};
    std::size_t history_count_{0};
};

}  // namespace ptv
