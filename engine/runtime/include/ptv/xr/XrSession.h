#pragma once

#include <volk.h>

// These match the target_compile_definitions in engine/runtime/CMakeLists.txt.
// Use #ifndef guards so we get no redefinition warnings when they are already
// set on the compile command line.
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif

// openxr_platform.h on Windows references LARGE_INTEGER (windows.h) and
// IUnknown (unknwn.h). Pull them in here so every translation unit that
// includes XrSession.h gets the platform types in the right order.
#ifdef XR_USE_PLATFORM_WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <unknwn.h>
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "ptv/render/VulkanContext.h"

namespace ptv {

/**
 * OpenXR session + Vulkan graphics binding.
 *
 * We deliberately name the class ``XrRuntime`` (not ``XrSession``) to avoid
 * colliding with the OpenXR C typedef ``XrSession``. The class owns:
 *   * xrInstance, xrSystem, xrSession
 *   * one action set ("gameplay") with left/right hand pose + trigger / grip
 *     float inputs + thumbstick vec2 + A/B/X/Y buttons
 *   * stage reference space
 *   * per-eye color swapchains
 *   * the frame loop (wait/begin/end, view locate, projection composition).
 */
class XrRuntime {
public:
    struct Views {
        XrView left{XR_TYPE_VIEW};
        XrView right{XR_TYPE_VIEW};
        XrTime predicted_display_time{0};
    };

    struct InputState {
        // Hand poses expressed in the runtime's stage reference space.
        // ``hand_*_valid`` is true only when the runtime reports the pose
        // as both tracked AND position/orientation valid.
        XrSpaceLocation hand_left{XR_TYPE_SPACE_LOCATION};
        XrSpaceLocation hand_right{XR_TYPE_SPACE_LOCATION};
        bool hand_left_valid{false};
        bool hand_right_valid{false};
        float trigger[2]{0.0f, 0.0f};   // [0]=left, [1]=right
        float grip[2]{0.0f, 0.0f};
        XrVector2f thumbstick[2]{};     // [0]=left, [1]=right
        bool button_a{false};
        bool button_b{false};
        bool button_x{false};
        bool button_y{false};
    };

    XrRuntime() = default;
    ~XrRuntime();

    XrRuntime(const XrRuntime&) = delete;
    XrRuntime& operator=(const XrRuntime&) = delete;

    /** Extensions OpenXR needs on the Vulkan instance/device. */
    static std::vector<std::string> required_vulkan_instance_extensions();
    static std::vector<std::string> required_vulkan_device_extensions(XrInstance instance,
                                                                      XrSystemId system);

    /** Combined init (kept for tests / non-XR-aware use; prefer the
     *  two-phase flow below when using XR_KHR_vulkan_enable2). */
    bool initialize(VulkanContext& vk, const std::string& app_name);

    /** Phase 1: create XR instance, system, view config and cache the Vulkan
     *  graphics requirements. Call this BEFORE Vulkan is initialized. */
    bool init_instance_and_system(const std::string& app_name);

    /** Phase 2: create session, reference space, action set, swapchains.
     *  Call AFTER VulkanContext::initialize_for_openxr. */
    bool init_session_and_swapchains(VulkanContext& vk);

    /** Runtime-reported min/max Vulkan API (0 if not yet queried). Packed
     *  into the low 32 bits using ``VK_MAKE_API_VERSION``. */
    uint32_t runtime_min_vulkan_api() const { return runtime_min_vk_api_; }
    uint32_t runtime_max_vulkan_api() const { return runtime_max_vk_api_; }

    void shutdown();

    bool poll_events();                     // returns false when the session is exiting
    bool begin_frame(Views& views);         // returns false when the frame should be skipped
    void end_frame_no_render();             // submit an empty frame (e.g. when not focused)
    void sync_input(InputState& out);

    // Per-eye swapchain resources exposed to the renderer.
    struct EyeImage {
        VkImage image{VK_NULL_HANDLE};
        uint32_t width{0};
        uint32_t height{0};
        XrFovf fov{};
        XrPosef pose{};
    };
    // Full render-and-submit cycle. ``render`` is invoked between acquire
    // and release, with the two eye images filled in. Returns false when
    // the frame should be skipped (session not running, shouldRender=false).
    using RenderCallback = std::function<void(const std::array<EyeImage, 2>&,
                                              XrTime predicted_display_time)>;
    bool render_frame(const RenderCallback& render);

    VkFormat color_format() const { return color_format_; }
    uint32_t eye_width() const {
        return view_config_views_[0].recommendedImageRectWidth;
    }
    uint32_t eye_height() const {
        return view_config_views_[0].recommendedImageRectHeight;
    }

    bool is_running() const { return is_running_; }

    XrInstance instance() const { return xr_instance_; }
    XrSystemId system() const { return xr_system_; }
    ::XrSession xr_session() const { return xr_session_; }

private:
    bool create_instance(const std::string& app_name);
    bool create_session(VulkanContext& vk);
    bool create_reference_space();
    bool create_action_set();
    bool create_swapchains();

    XrInstance xr_instance_{XR_NULL_HANDLE};
    XrSystemId xr_system_{XR_NULL_SYSTEM_ID};
    ::XrSession xr_session_{XR_NULL_HANDLE};
    XrSpace stage_space_{XR_NULL_HANDLE};
    XrSpace hand_space_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};

    XrActionSet action_set_{XR_NULL_HANDLE};
    XrAction hand_pose_action_{XR_NULL_HANDLE};
    XrAction trigger_action_{XR_NULL_HANDLE};
    XrAction grip_action_{XR_NULL_HANDLE};
    XrAction thumbstick_action_{XR_NULL_HANDLE};
    XrAction button_a_action_{XR_NULL_HANDLE};
    XrAction button_b_action_{XR_NULL_HANDLE};
    XrPath hand_paths_[2]{XR_NULL_PATH, XR_NULL_PATH};

    std::vector<XrSwapchain> color_swapchains_;
    std::vector<std::vector<XrSwapchainImageVulkan2KHR>> color_images_;
    VkFormat color_format_{VK_FORMAT_R8G8B8A8_SRGB};

    XrSessionState session_state_{XR_SESSION_STATE_UNKNOWN};
    XrViewConfigurationType view_config_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    std::array<XrViewConfigurationView, 2> view_config_views_{};
    bool is_running_{false};
    uint32_t runtime_min_vk_api_{0};
    uint32_t runtime_max_vk_api_{0};
    // Updated inside ``begin_frame`` / ``render_frame``; used by
    // ``sync_input`` when it calls ``xrLocateSpace`` for the hand poses.
    XrTime last_predicted_display_time_{0};
};

}  // namespace ptv
