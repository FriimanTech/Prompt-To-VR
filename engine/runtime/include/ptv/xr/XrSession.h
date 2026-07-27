#pragma once

#include <volk.h>

// The graphics API is Vulkan on BOTH desktop and Quest. The platform macro
// (XR_USE_PLATFORM_WIN32 vs XR_USE_PLATFORM_ANDROID) is supplied by the
// per-target CMakeLists (engine/runtime for desktop, engine/android for
// Quest). Only fall back to Win32 here when neither was set AND we're on a
// Windows compiler, so the header never forces Win32 into an Android build.
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#if !defined(XR_USE_PLATFORM_WIN32) && !defined(XR_USE_PLATFORM_ANDROID)
#  if defined(_WIN32)
#    define XR_USE_PLATFORM_WIN32
#  elif defined(__ANDROID__)
#    define XR_USE_PLATFORM_ANDROID
#  endif
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

// openxr_platform.h on Android references the JNI JavaVM type.
#ifdef XR_USE_PLATFORM_ANDROID
#  include <jni.h>
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
        // **Grip** poses expressed in the runtime's stage reference space.
        //
        // OpenXR's ``/grip/pose`` is "the user's palm centroid when
        // making a fist". On Quest Touch the grip handle is angled
        // ~30-40 deg below the controller body, so grip's -Z axis is
        // aligned with the user's HAND, NOT with the controller body.
        // We use grip pose for "where is the hand" -- visualization,
        // grab interaction, kinematic follow.
        //
        // ``hand_*_valid`` is true only when the runtime reports the pose
        // as both tracked AND position/orientation valid.
        XrSpaceLocation hand_left{XR_TYPE_SPACE_LOCATION};
        XrSpaceLocation hand_right{XR_TYPE_SPACE_LOCATION};
        bool hand_left_valid{false};
        bool hand_right_valid{false};
        // **Aim** poses expressed in the same stage reference space.
        //
        // OpenXR's ``/aim/pose`` is "what direction is the user
        // pointing the input source", with -Z aligned with the
        // controller's *body* (not the angled grip handle). This is
        // what locomotion code should use whenever it asks "which way
        // is the controller pointing?" -- e.g. controller-relative
        // sliding direction, teleport arc origin, ray casts.
        XrSpaceLocation aim_left{XR_TYPE_SPACE_LOCATION};
        XrSpaceLocation aim_right{XR_TYPE_SPACE_LOCATION};
        bool aim_left_valid{false};
        bool aim_right_valid{false};
        // HMD (head) pose expressed in the **same** stage reference space
        // as the hand poses. This is OpenXR's ``XR_REFERENCE_SPACE_TYPE_VIEW``
        // located against the stage space at the predicted display time --
        // i.e. exactly the space-pair Meta and OpenXR recommend for
        // gameplay logic that must stay coordinate-compatible with the
        // hands. Locomotion code MUST consume this field every frame; do
        // not cache or fall back to a fixed world-space forward vector.
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        bool head_valid{false};
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

    /** Phase 1 of the XR frame loop.
     *
     *  Calls ``xrWaitFrame`` + ``xrBeginFrame`` and stores the predicted
     *  display time so the *very next* ``sync_input`` call locates head /
     *  hand spaces at the *current* frame's predicted time -- not at the
     *  previous frame's time, which is what happens when ``render_frame``
     *  is the only thing calling ``xrWaitFrame``.
     *
     *  Returns ``true`` when the runtime says we should render. When it
     *  returns ``false`` it has already submitted an empty end-frame so
     *  the caller only needs to skip rendering for this iteration --
     *  do **not** call ``render_frame`` afterwards.
     *
     *  When this returns ``true``, the caller MUST eventually call
     *  ``render_frame`` (or ``submit_pending_empty_frame``) to balance
     *  the wait/begin pair. Calling ``render_frame`` after this will
     *  reuse the pending state and skip its own ``xrWaitFrame``. */
    bool begin_xr_frame();

    /** If a ``begin_xr_frame`` is pending and the caller decided not to
     *  render this iteration after all, submit an empty end-frame to
     *  keep the wait/begin pairing balanced. Safe to call when no
     *  begin is pending (no-op). */
    void submit_pending_empty_frame();
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
    // ``XR_REFERENCE_SPACE_TYPE_VIEW``. Located against ``stage_space_``
    // every frame in ``sync_input`` so locomotion always sees the latest
    // HMD pose in the same coordinate system as the hand poses (no
    // mixing of LOCAL / STAGE / VIEW). Per OpenXR spec the runtime
    // updates VIEW continuously to track the user's head.
    XrSpace view_space_{XR_NULL_HANDLE};
    // GRIP-pose action spaces (palm-centric, used for hand visualization
    // / grab interaction). See InputState::hand_*.
    XrSpace hand_space_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};
    // AIM-pose action spaces (controller-body forward, used for slide
    // direction / teleport arc / ray casts). See InputState::aim_*.
    XrSpace aim_space_[2]{XR_NULL_HANDLE, XR_NULL_HANDLE};

    XrActionSet action_set_{XR_NULL_HANDLE};
    XrAction hand_pose_action_{XR_NULL_HANDLE};   // bound to /grip/pose
    XrAction aim_pose_action_{XR_NULL_HANDLE};    // bound to /aim/pose
    XrAction trigger_action_{XR_NULL_HANDLE};
    XrAction grip_action_{XR_NULL_HANDLE};
    XrAction thumbstick_action_{XR_NULL_HANDLE};
    XrAction button_a_action_{XR_NULL_HANDLE};   // right controller A
    XrAction button_b_action_{XR_NULL_HANDLE};   // right controller B
    XrAction button_x_action_{XR_NULL_HANDLE};   // left controller X
    XrAction button_y_action_{XR_NULL_HANDLE};   // left controller Y
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
    // Updated inside ``begin_frame`` / ``begin_xr_frame`` / ``render_frame``;
    // used by ``sync_input`` when it calls ``xrLocateSpace`` for the head /
    // hand poses.
    XrTime last_predicted_display_time_{0};
    // True between ``begin_xr_frame`` and the matching ``render_frame`` /
    // ``submit_pending_empty_frame``. When set, ``render_frame`` reuses
    // ``pending_frame_state_`` instead of issuing its own
    // ``xrWaitFrame`` + ``xrBeginFrame``.
    bool frame_started_{false};
    XrFrameState pending_frame_state_{XR_TYPE_FRAME_STATE};
};

}  // namespace ptv
