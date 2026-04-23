#pragma once

#include <spdlog/spdlog.h>

namespace ptv {

inline spdlog::logger& log() {
    static auto logger = [] {
        auto l = spdlog::default_logger()->clone("ptv");
        spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
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
