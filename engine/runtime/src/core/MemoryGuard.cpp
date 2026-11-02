#include "ptv/core/MemoryGuard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__ANDROID__)
#include <malloc.h>
#endif
#if defined(__ANDROID__) || defined(__linux__)
#include <unistd.h>
#endif

#include "ptv/core/Logger.h"

namespace ptv {

std::atomic<bool> MemoryGuard::low_memory_signal_{false};

namespace {

/** First "<key>: <n> kB" value in a /proc file, in MB (0 when absent). */
float proc_kb_field_mb(const char* path, const char* key) {
    std::ifstream f(path);
    if (!f) return 0.0f;
    std::string line;
    const std::size_t klen = std::strlen(key);
    while (std::getline(f, line)) {
        if (line.compare(0, klen, key) == 0) {
            std::istringstream ss(line.substr(klen));
            double kb = 0.0;
            ss >> kb;
            return static_cast<float>(kb / 1024.0);
        }
    }
    return 0.0f;
}

}  // namespace

MemoryGuard::~MemoryGuard() { stop(); }

void MemoryGuard::configure(const ScenePerformance& perf) {
    target_mb_ = perf.memory_target_mb > 0.0f ? perf.memory_target_mb : 4608.0f;
    ceiling_mb_ = perf.memory_ceiling_mb > 0.0f ? perf.memory_ceiling_mb : 5120.0f;
    limit_mb_ = perf.memory_limit_mb > 0.0f ? perf.memory_limit_mb : 5888.0f;
    // 4.25 / 4.5 / 4.8 GiB for the default 4.5 GiB target, scaled with it.
    green_below_mb_ = target_mb_ * (4352.0f / 4608.0f);
    red_from_mb_ = target_mb_;
    critical_from_mb_ = target_mb_ * (4915.0f / 4608.0f);
}

void MemoryGuard::set_simulated_pressure(float scale) {
    pressure_scale_ = std::clamp(scale, 0.001f, 1.0f);
    PTV_WARN("mem: simulated pressure -- thresholds scaled to {:.0f}%", pressure_scale_ * 100.0f);
}

void MemoryGuard::notify_low_memory() {
    low_memory_signal_.store(true);
    PTV_WARN("mem: APP_CMD_LOW_MEMORY received");
}

void MemoryGuard::start() {
    if (running_) return;
    stop_ = false;
    running_ = true;
    sampler_ = std::thread([this] { sampler_main(); });
}

void MemoryGuard::stop() {
    if (!running_) return;
    stop_ = true;
    if (sampler_.joinable()) sampler_.join();
    running_ = false;
}

void MemoryGuard::sampler_main() {
    using namespace std::chrono_literals;
    while (!stop_.load()) {
#if defined(__ANDROID__) || defined(__linux__)
        // smaps_rollup gives the process PSS without walking every mapping;
        // the Quest exposes no VK_EXT_memory_budget, so this is the number
        // Horizon OS itself judges the app by.
        float pss = proc_kb_field_mb("/proc/self/smaps_rollup", "Pss:");
        if (pss <= 0.0f) pss = proc_kb_field_mb("/proc/self/status", "VmRSS:");
        pss_mb_.store(pss);
        avail_mb_.store(proc_kb_field_mb("/proc/meminfo", "MemAvailable:"));
        // GPU memory the process holds but has not mapped is not in smaps;
        // Qualcomm's KGSL driver reports it per process (bytes). Unreadable
        // (SELinux) -> -1, and the engine's own GPU accounting stands in.
        {
            char path[96];
            std::snprintf(path, sizeof(path), "/sys/class/kgsl/kgsl/proc/%d/gpumem_unmapped",
                          static_cast<int>(getpid()));
            std::ifstream f(path);
            double bytes = -1.0;
            if (f && (f >> bytes)) {
                gpu_unmapped_mb_.store(static_cast<float>(bytes / (1024.0 * 1024.0)));
            } else {
                gpu_unmapped_mb_.store(-1.0f);
            }
        }
#endif
        for (int i = 0; i < 10 && !stop_.load(); ++i) std::this_thread::sleep_for(100ms);
    }
}

bool MemoryGuard::update(const Accounting& acc) {
    last_ = acc;
    const double engine_mb = acc.texture_mb + acc.mesh_mb + acc.targets_mb + acc.frame_mb;
    // smaps PSS covers code, heap and mapped GPU buffers; unmapped GPU memory
    // comes from KGSL when readable, otherwise from the engine's own exact
    // GPU accounting. Then add what is already queued to load.
    const float gpu = gpu_unmapped_mb_.load();
    const float base = pss_mb_.load() + (gpu >= 0.0f ? gpu : static_cast<float>(engine_mb));
    total_mb_ = base;
    projected_mb_ = base + static_cast<float>(acc.reserved_mb);

    const float avail = avail_mb_.load();
    const bool low_signal = low_memory_signal_.exchange(false);
    Level want = Level::Green;
    if (projected_mb_ >= threshold(critical_from_mb_)) {
        want = Level::Critical;
    } else if (projected_mb_ >= threshold(red_from_mb_) || low_signal ||
               (avail > 0.0f && avail < kMinAvailMb * pressure_scale_)) {
        want = Level::Red;
    } else if (projected_mb_ >= threshold(green_below_mb_)) {
        want = Level::Yellow;
    }

    Level next = level_;
    if (want > level_) {
        next = want;
    } else if (want < level_) {
        // Step down only with a clear margin below the current level's floor.
        const float floor = level_ == Level::Critical ? threshold(critical_from_mb_)
                            : level_ == Level::Red    ? threshold(red_from_mb_)
                                                      : threshold(green_below_mb_);
        if (projected_mb_ < floor - kHysteresisMb * pressure_scale_) {
            next = static_cast<Level>(static_cast<int>(level_) - 1);
        }
    }
    if (next == level_) return false;
    const Level prev = level_;
    level_ = next;
    if (next >= Level::Red) {
        PTV_ERROR("mem: level {} (projected {:.0f} MB, pss {:.0f}, target {:.0f}, ceiling "
                  "{:.0f}, limit {:.0f}); top consumers: textures {:.0f} MB, meshes {:.0f} MB, "
                  "render targets {:.0f} MB, queued loads {:.0f} MB",
                  level_name(next), projected_mb_, pss_mb_.load(), target_mb_, ceiling_mb_,
                  limit_mb_, acc.texture_mb, acc.mesh_mb, acc.targets_mb, acc.reserved_mb);
    } else {
        PTV_INFO("mem: level {} -> {} (projected {:.0f} MB)", level_name(prev), level_name(next),
                 projected_mb_);
    }
    return true;
}

const char* MemoryGuard::level_name(Level l) {
    switch (l) {
        case Level::Green: return "green";
        case Level::Yellow: return "yellow";
        case Level::Red: return "red";
        case Level::Critical: return "critical";
    }
    return "?";
}

std::string MemoryGuard::telemetry_line() const {
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "mem: pss %.0f MB (target %.0f, ceiling %.0f, limit %.0f) | projected %.0f | "
                  "smaps %.0f | gpu_unmapped %.0f | tex %.0f | mesh %.0f | targets %.0f | "
                  "reserved %.0f | sys_avail %.0f | level %s | evict %d",
                  total_mb_, target_mb_, ceiling_mb_, limit_mb_, projected_mb_, pss_mb_.load(),
                  gpu_unmapped_mb_.load(), last_.texture_mb, last_.mesh_mb, last_.targets_mb,
                  last_.reserved_mb, avail_mb_.load(), level_name(level_), evictions_);
    return buf;
}

void MemoryGuard::purge_heap() {
#if defined(__ANDROID__)
#if defined(M_PURGE_ALL)
    mallopt(M_PURGE_ALL, 0);
#elif defined(M_PURGE)
    mallopt(M_PURGE, 0);
#endif
#endif
}

}  // namespace ptv
