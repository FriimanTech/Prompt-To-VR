#pragma once

#include "ptv/scene/Scene.h"

namespace ptv {

/**
 * Runtime adaptive quality controller.
 *
 * The build-time budget (``ScenePerformance``, produced by the
 * ``asset_optimization`` pipeline stage) is only an ESTIMATE of what a scene
 * costs. The governor closes the loop on the real device: it watches measured
 * frame time and trades visual quality for frame rate until the headset holds
 * its target refresh, then gives the quality back when there is headroom.
 *
 * This is what lets an arbitrarily large prompted game stay playable without
 * the pipeline hard-capping how much content a prompt may ask for. A compact
 * scene never trips the governor at all and runs at full quality.
 *
 * Escalation ladder, cheapest perceptual cost first:
 *   1. raise the LOD bias   (distant meshes drop to a coarser level sooner)
 *   2. shrink the cull distance (far objects stop drawing)
 *   3. raise the foveation level (periphery renders at lower resolution)
 * Relaxation walks the same ladder in reverse.
 *
 * Hysteresis (``escalate_after_bad_frames`` / ``relax_after_good_frames``,
 * plus a cooldown between steps) keeps it from oscillating: quality drops
 * quickly when the frame rate collapses but returns slowly and steadily.
 */
class QualityGovernor {
public:
    struct State {
        float lod_bias{0.0f};
        float cull_distance_m{100000.0f};
        int foveation_level{0};
    };

    /** Adopt a scene's budget. Resets to that scene's baseline quality. */
    void configure(const ScenePerformance& perf);

    /**
     * Feed one frame's wall-clock duration.
     *
     * @return true when the quality state changed and the caller should push
     *         the new values into the renderer / XR session.
     */
    bool update(float frame_seconds);

    const State& state() const { return state_; }
    bool enabled() const { return enabled_; }
    /** How many escalation steps deep we currently are (0 = full quality). */
    int pressure() const { return pressure_; }

private:
    bool apply_pressure(int pressure);

    bool enabled_{false};
    State state_{};

    // Baseline (full-quality) values from the scene budget.
    float base_cull_distance_m_{100000.0f};
    int base_foveation_level_{0};

    // Bounds the governor may move within.
    float max_lod_bias_{2.0f};
    float min_cull_distance_m_{25.0f};
    int max_foveation_level_{3};

    float frame_budget_s_{1.0f / 72.0f};
    int escalate_after_{30};
    int relax_after_{180};

    int bad_frames_{0};
    int good_frames_{0};
    int pressure_{0};
    int max_pressure_{0};
};

}  // namespace ptv
