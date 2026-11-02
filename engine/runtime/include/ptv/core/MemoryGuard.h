#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "ptv/scene/Scene.h"

namespace ptv {

/**
 * Always-on memory protection for the Quest 3 (every APK, never disabled).
 *
 * Meta's limit for an immersive app is 5.75 GiB PSS; the engine plans for a
 * 4.5 GiB steady state and a 5.0 GiB peak (``ScenePerformance::memory_*``).
 * The guard watches, once a second on a background thread, the real PSS
 * (``/proc/self/smaps_rollup``) and the system's ``MemAvailable``, adds the
 * engine's own exact accounting (textures, meshes, render targets) and the
 * bytes of loads already queued, and acts on that PROJECTION -- before usage
 * climbs, not after Android complains.
 *
 *   green    projected < 4.25 GiB   normal
 *   yellow   4.25 - 4.5 GiB         no speculative prefetch; evict invisible
 *                                   streamable data down to 4.0 GiB
 *   red      4.5 - 4.8 GiB, or MemAvailable < 512 MB, or APP_CMD_LOW_MEMORY
 *                                   evict everything invisible above the
 *                                   always-resident floor, defer high-detail
 *                                   loads, return freed heap to the OS, log
 *                                   the top consumers
 *   critical >= 4.8 GiB             also draw visible objects at their
 *                                   resident lower LODs (a visible quality
 *                                   drop, never a crash)
 *
 * Levels fall back only once the projection is 128 MB under the threshold.
 * It reacts to memory only -- frame time is QualityGovernor's -- and it never
 * touches gameplay: entities, colliders, navigation, AI and game state stay.
 */
class MemoryGuard {
public:
    enum class Level { Green = 0, Yellow = 1, Red = 2, Critical = 3 };

    /** The engine's own exact accounting, fed every frame. */
    struct Accounting {
        double texture_mb{0.0};
        double mesh_mb{0.0};
        double targets_mb{0.0};
        double frame_mb{0.0};
        /** Bytes of loads queued but not yet resident (reservations). */
        double reserved_mb{0.0};
    };

    ~MemoryGuard();

    /** Thresholds from the scene's memory targets. */
    void configure(const ScenePerformance& perf);
    /** Start the 1 Hz sampler (Android: smaps_rollup + meminfo). */
    void start();
    void stop();

    /** APP_CMD_LOW_MEMORY from the native glue (a late, extra signal). */
    static void notify_low_memory();
    /** Verification only: scale every threshold (0.05 = trip at 5%). */
    void set_simulated_pressure(float scale);

    /** Recompute the level from the latest sample and this frame's
     *  accounting. Returns true when the level changed. */
    bool update(const Accounting& acc);
    Level level() const { return level_; }
    static const char* level_name(Level l);

    float pss_mb() const { return pss_mb_.load(); }
    float avail_mb() const { return avail_mb_.load(); }
    float projected_mb() const { return projected_mb_; }
    void add_evictions(int n) { evictions_ += n; }

    /** The 1 Hz ``mem:`` telemetry line. */
    std::string telemetry_line() const;

    /** Return freed native heap pages to the OS (mallopt M_PURGE_ALL on
     *  Android; no-op elsewhere). */
    static void purge_heap();

private:
    void sampler_main();
    float threshold(float mb) const { return mb * pressure_scale_; }

    float target_mb_{4608.0f};
    float ceiling_mb_{5120.0f};
    float limit_mb_{5888.0f};
    float green_below_mb_{4352.0f};
    float red_from_mb_{4608.0f};
    float critical_from_mb_{4915.0f};
    float pressure_scale_{1.0f};
    static constexpr float kHysteresisMb = 128.0f;
    static constexpr float kMinAvailMb = 512.0f;

    std::atomic<float> pss_mb_{0.0f};
    std::atomic<float> avail_mb_{0.0f};
    std::atomic<float> gpu_unmapped_mb_{-1.0f};
    static std::atomic<bool> low_memory_signal_;
    float projected_mb_{0.0f};
    /** smaps PSS + unmapped GPU memory: what Horizon OS charges the app. */
    float total_mb_{0.0f};
    Accounting last_{};
    Level level_{Level::Green};
    int evictions_{0};

    std::thread sampler_;
    std::atomic<bool> stop_{false};
    bool running_{false};
};

}  // namespace ptv
