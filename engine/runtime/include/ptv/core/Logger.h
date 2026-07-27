#pragma once

#include <spdlog/spdlog.h>

#if defined(__ANDROID__)
#include <memory>

#include <spdlog/sinks/android_sink.h>
#endif

namespace ptv {

inline spdlog::logger& log() {
    static auto logger = [] {
#if defined(__ANDROID__)
        // On Quest, stdout/stderr are NOT routed to logcat, so the desktop
        // stdout sink would make the engine effectively silent on-device.
        // Route logs through the NDK __android_log_write sink under the tag
        // "ptv" so they are visible via `adb logcat -s ptv`.
        auto sink = std::make_shared<spdlog::sinks::android_sink_mt>("ptv");
        auto l = std::make_shared<spdlog::logger>("ptv", sink);
        spdlog::set_default_logger(l);
        l->set_pattern("[%^%l%$] %v");  // logcat adds its own timestamp/pid
#else
        auto l = spdlog::default_logger()->clone("ptv");
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
#endif
        // Flush on every log level so if the program aborts mid-XR-init
        // we still see exactly which step was last executed. Minimal
        // overhead because the skeleton does not log in hot loops.
        spdlog::flush_on(spdlog::level::trace);
        return l;
    }();
    return *logger;
}

#define PTV_INFO(...)  ::ptv::log().info(__VA_ARGS__)
#define PTV_WARN(...)  ::ptv::log().warn(__VA_ARGS__)
#define PTV_ERROR(...) ::ptv::log().error(__VA_ARGS__)
#define PTV_DEBUG(...) ::ptv::log().debug(__VA_ARGS__)

}  // namespace ptv
