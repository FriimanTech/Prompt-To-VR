#include "ptv/xr/XrSession.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

#include "ptv/core/Logger.h"

#if defined(XR_USE_PLATFORM_ANDROID)
#include <android_native_app_glue.h>

#include "../platform/android/AndroidPlatform.h"
#endif

namespace ptv {

namespace {

bool xr_ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE]{};
    xrResultToString(XR_NULL_HANDLE, r, buf);  // instance may be null; falls back to static names
    PTV_ERROR("OpenXR {} failed: {}", what, buf);
    return false;
}

std::vector<std::string> split_space_delim(const char* s) {
    std::vector<std::string> out;
    if (!s) return out;
    std::stringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

}  // namespace

XrRuntime::~XrRuntime() { shutdown(); }

std::vector<std::string> XrRuntime::required_vulkan_instance_extensions() {
    // Default safe list: OpenXR's VK_KHR_vulkan_enable2 collapses this to just
    // whatever the runtime itself needs. At ``create_session`` time we will
    // query the exact list per runtime; here we just list the common ones.
    return {
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_external_fence_capabilities",
    };
}

std::vector<std::string> XrRuntime::required_vulkan_device_extensions(
    XrInstance /*instance*/, XrSystemId /*system*/) {
    // Most Windows runtimes require these for interop.
    return {
        "VK_KHR_external_memory",
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_semaphore_win32",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_external_fence",
        "VK_KHR_external_fence_win32",
    };
}

bool XrRuntime::initialize(VulkanContext& vk, const std::string& app_name) {
    if (!init_instance_and_system(app_name)) return false;
    if (!init_session_and_swapchains(vk)) return false;
    return true;
}

bool XrRuntime::init_instance_and_system(const std::string& app_name) {
    PTV_INFO("XR: creating instance...");
    if (!create_instance(app_name)) return false;
    PTV_INFO("XR: instance ok");

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_ok(xrGetSystem(xr_instance_, &sgi, &xr_system_), "xrGetSystem")) {
        PTV_ERROR("No OpenXR system found. Is a runtime installed (Meta Quest Link active)?");
        return false;
    }
    PTV_INFO("XR: got system id {}", static_cast<uint64_t>(xr_system_));

    uint32_t vcount = 0;
    xr_ok(xrEnumerateViewConfigurationViews(
              xr_instance_, xr_system_, view_config_, 0, &vcount, nullptr),
          "xrEnumerateViewConfigurationViews");
    if (vcount != 2) {
        PTV_ERROR("Expected stereo (2 views), got {}", vcount);
        return false;
    }
    view_config_views_.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xr_ok(xrEnumerateViewConfigurationViews(xr_instance_, xr_system_, view_config_,
                                            vcount, &vcount, view_config_views_.data()),
          "xrEnumerateViewConfigurationViews#2");
    PTV_INFO("XR: view config {}x{} per eye",
             view_config_views_[0].recommendedImageRectWidth,
             view_config_views_[0].recommendedImageRectHeight);

    // Query and cache the Vulkan graphics requirements so VulkanContext
    // can be told the accepted API range.
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn = nullptr;
    xrGetInstanceProcAddr(xr_instance_, "xrGetVulkanGraphicsRequirements2KHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfn));
    if (pfn) {
        if (XR_SUCCEEDED(pfn(xr_instance_, xr_system_, &req))) {
            // OpenXR packs the Vulkan-formatted version into the low 32 bits.
            runtime_min_vk_api_ = static_cast<uint32_t>(req.minApiVersionSupported);
            runtime_max_vk_api_ = static_cast<uint32_t>(req.maxApiVersionSupported);
            PTV_INFO("XR: runtime supports Vulkan API {}.{} .. {}.{}",
                     VK_VERSION_MAJOR(runtime_min_vk_api_),
                     VK_VERSION_MINOR(runtime_min_vk_api_),
                     VK_VERSION_MAJOR(runtime_max_vk_api_),
                     VK_VERSION_MINOR(runtime_max_vk_api_));
        }
    }
    return true;
}

bool XrRuntime::init_session_and_swapchains(VulkanContext& vk) {
    PTV_INFO("XR: creating session (Vulkan graphics binding)...");
    if (!create_session(vk)) return false;
    PTV_INFO("XR: session ok");

    PTV_INFO("XR: creating reference space...");
    if (!create_reference_space()) return false;
    PTV_INFO("XR: reference space ok");

    PTV_INFO("XR: creating action set...");
    if (!create_action_set()) return false;
    PTV_INFO("XR: action set ok");

    // Multiview needs the Vulkan feature; FFR through a fragment density map
    // needs both the Vulkan extension and XR_FB_foveation_vulkan.
    multiview_ = swap_opts_.multiview && vk.multiview_enabled();
    fdm_ = swap_opts_.fragment_density_map && foveation_supported_ &&
           foveation_vulkan_supported_ && vk.fragment_density_map_enabled();
    PTV_INFO("XR: creating swapchains (multiview={} fdm={} scale {:.2f})...", multiview_,
             fdm_, swap_opts_.render_scale_ceiling);
    if (!create_swapchains()) return false;
    PTV_INFO("XR: swapchains ok");

    if (refresh_rate_supported_) {
        PFN_xrEnumerateDisplayRefreshRatesFB enum_rates = nullptr;
        PFN_xrGetDisplayRefreshRateFB get_rate = nullptr;
        xrGetInstanceProcAddr(xr_instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&enum_rates));
        xrGetInstanceProcAddr(xr_instance_, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_rate));
        uint32_t n = 0;
        if (enum_rates && XR_SUCCEEDED(enum_rates(xr_session_, 0, &n, nullptr)) && n > 0) {
            refresh_rates_.resize(n);
            enum_rates(xr_session_, n, &n, refresh_rates_.data());
        }
        if (get_rate) get_rate(xr_session_, &refresh_hz_);
        std::string rates;
        for (float r : refresh_rates_) rates += std::to_string(static_cast<int>(r + 0.5f)) + " ";
        PTV_INFO("XR: display refresh {:.0f} Hz (supported: {})", refresh_hz_, rates);
        if (swap_opts_.refresh_hz > 0.0f) request_refresh_rate(swap_opts_.refresh_hz);
    }

    PTV_INFO("OpenXR runtime ready ({}x{} per eye, swapchain {}x{})",
             view_config_views_[0].recommendedImageRectWidth,
             view_config_views_[0].recommendedImageRectHeight, swap_width_, swap_height_);
    return true;
}

bool XrRuntime::request_refresh_rate(float hz) {
    if (!refresh_rate_supported_ || xr_session_ == XR_NULL_HANDLE || hz <= 0.0f) return false;
    // Pick the closest supported rate not above the request (72 is always
    // available on Quest).
    float best = 0.0f;
    for (float r : refresh_rates_) {
        if (r <= hz + 0.5f && r > best) best = r;
    }
    if (best <= 0.0f) return false;
    PFN_xrRequestDisplayRefreshRateFB req = nullptr;
    xrGetInstanceProcAddr(xr_instance_, "xrRequestDisplayRefreshRateFB",
                          reinterpret_cast<PFN_xrVoidFunction*>(&req));
    if (!req || !XR_SUCCEEDED(req(xr_session_, best))) {
        PTV_WARN("XR: could not request {:.0f} Hz", best);
        return false;
    }
    refresh_hz_ = best;
    PTV_INFO("XR: display refresh -> {:.0f} Hz", best);
    return true;
}

void XrRuntime::set_render_scale(float s) {
    render_scale_ = std::clamp(s, 0.5f, 1.0f);
}

bool XrRuntime::pending_views(std::array<XrView, 2>& out) const {
    if (!frame_started_ || !pending_views_valid_) return false;
    out = pending_views_;
    return true;
}

bool XrRuntime::create_instance(const std::string& app_name) {
#if defined(XR_USE_PLATFORM_ANDROID)
    // On Android the Khronos OpenXR loader MUST be initialised with the
    // JavaVM + activity context BEFORE any other OpenXR call (Meta requires
    // loader >= 1.0.34). See mobile-openxr docs / XR_KHR_loader_init_android.
    {
        PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
        if (XR_SUCCEEDED(xrGetInstanceProcAddr(
                XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR))) &&
            xrInitializeLoaderKHR != nullptr) {
            android_app* app = current_android_app();
            if (app && app->activity) {
                XrLoaderInitInfoAndroidKHR init{
                    XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
                init.applicationVM = app->activity->vm;
                init.applicationContext = app->activity->clazz;
                xrInitializeLoaderKHR(
                    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init));
            } else {
                PTV_ERROR("XR(Android): no android_app/activity for loader init");
                return false;
            }
        } else {
            PTV_ERROR("XR(Android): xrInitializeLoaderKHR not resolvable");
            return false;
        }
    }
#endif
    // Enumerate the extensions the runtime offers so we can log any that we
    // ask for and don't get, and so we can diagnose missing runtime installs.
    uint32_t ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> avail(
        ext_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count,
                                           avail.data());
    PTV_INFO("XR: runtime exposes {} instance extensions", ext_count);
    auto has_extension = [&avail](const char* name) {
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    if (!has_extension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        PTV_ERROR("XR: runtime does not expose {} (required).",
                  XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        return false;
    }

    std::vector<const char*> exts{
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
    };
#if defined(XR_USE_PLATFORM_ANDROID)
    // Required so xrCreateInstance receives the Android VM + activity context.
    exts.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif

    // Fixed foveated rendering: render the periphery at lower resolution.
    // This is one of the cheapest large GPU wins on a Quest (the player does
    // not perceive the edge blur), so we opt in whenever the runtime offers
    // the full trio. All three are OPTIONAL -- PC VR runtimes generally do not
    // expose them and the engine simply runs without foveation.
    foveation_supported_ =
        has_extension(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) &&
        has_extension(XR_FB_FOVEATION_EXTENSION_NAME) &&
        has_extension(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
    if (foveation_supported_) {
        exts.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
        exts.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
        exts.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
        PTV_INFO("XR: fixed foveated rendering extensions available");
        // Vulkan apps get FFR through a fragment density map the runtime
        // writes per swapchain image; without it the level changes nothing.
        foveation_vulkan_supported_ = has_extension(XR_FB_FOVEATION_VULKAN_EXTENSION_NAME);
        if (foveation_vulkan_supported_) exts.push_back(XR_FB_FOVEATION_VULKAN_EXTENSION_NAME);
    }
    refresh_rate_supported_ = has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (refresh_rate_supported_) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
#if defined(XR_USE_PLATFORM_ANDROID)
    XrInstanceCreateInfoAndroidKHR android_ci{
        XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    if (android_app* app = current_android_app(); app && app->activity) {
        android_ci.applicationVM = app->activity->vm;
        android_ci.applicationActivity = app->activity->clazz;
        ici.next = &android_ci;
    }
#endif
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.enabledExtensionNames = exts.data();
    std::strncpy(ici.applicationInfo.applicationName,
                 app_name.c_str(),
                 sizeof(ici.applicationInfo.applicationName) - 1);
    ici.applicationInfo.applicationVersion = 1;
    std::strncpy(ici.applicationInfo.engineName, "ptv_engine",
                 sizeof(ici.applicationInfo.engineName) - 1);
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    return xr_ok(xrCreateInstance(&ici, &xr_instance_), "xrCreateInstance");
}

bool XrRuntime::create_session(VulkanContext& vk) {
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn = nullptr;
    xrGetInstanceProcAddr(xr_instance_, "xrGetVulkanGraphicsRequirements2KHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfn));
    if (pfn) {
        XrResult gr = pfn(xr_instance_, xr_system_, &req);
        if (XR_SUCCEEDED(gr)) {
            PTV_INFO("XR: runtime supports Vulkan API {}.{} .. {}.{}",
                     XR_VERSION_MAJOR(req.minApiVersionSupported),
                     XR_VERSION_MINOR(req.minApiVersionSupported),
                     XR_VERSION_MAJOR(req.maxApiVersionSupported),
                     XR_VERSION_MINOR(req.maxApiVersionSupported));
        } else {
            xr_ok(gr, "xrGetVulkanGraphicsRequirements2KHR");
        }
    } else {
        PTV_WARN("XR: xrGetVulkanGraphicsRequirements2KHR not resolvable; continuing.");
    }

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vk.instance();
    binding.physicalDevice = vk.physical_device();
    binding.device = vk.device();
    binding.queueFamilyIndex = vk.graphics_queue_family();
    binding.queueIndex = 0;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.systemId = xr_system_;
    sci.next = &binding;
    PTV_INFO("XR: calling xrCreateSession (graphics binding Vulkan2 KHR)...");
    return xr_ok(xrCreateSession(xr_instance_, &sci, &xr_session_), "xrCreateSession");
}

bool XrRuntime::create_reference_space() {
    // Stage / floor reference space: the engine's "world" for gameplay,
    // hand poses, and movement. Falls back to LOCAL if the runtime can't
    // produce a stage (e.g. seated-only setups).
    XrReferenceSpaceCreateInfo ri{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ri.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    ri.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    if (!xr_ok(xrCreateReferenceSpace(xr_session_, &ri, &stage_space_),
               "xrCreateReferenceSpace(STAGE)")) {
        ri.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!xr_ok(xrCreateReferenceSpace(xr_session_, &ri, &stage_space_),
                   "xrCreateReferenceSpace(LOCAL)")) {
            return false;
        }
    }

    // VIEW reference space: tracks the HMD's center-eye pose. We locate
    // it against ``stage_space_`` every frame to feed locomotion the
    // freshest head pose in the same coordinate system as the hands.
    // OpenXR spec says VIEW is supported by every conformant runtime.
    XrReferenceSpaceCreateInfo vri{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vri.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vri.poseInReferenceSpace = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    if (!xr_ok(xrCreateReferenceSpace(xr_session_, &vri, &view_space_),
               "xrCreateReferenceSpace(VIEW)")) {
        // Non-fatal: locomotion will fall back to identity head, but
        // the rest of the session can still run.
        view_space_ = XR_NULL_HANDLE;
    }
    return true;
}

bool XrRuntime::create_action_set() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(asci.actionSetName, "gameplay", sizeof(asci.actionSetName) - 1);
    std::strncpy(asci.localizedActionSetName, "Gameplay",
                 sizeof(asci.localizedActionSetName) - 1);
    asci.priority = 0;
    if (!xr_ok(xrCreateActionSet(xr_instance_, &asci, &action_set_),
               "xrCreateActionSet")) {
        return false;
    }

    xrStringToPath(xr_instance_, "/user/hand/left", &hand_paths_[0]);
    xrStringToPath(xr_instance_, "/user/hand/right", &hand_paths_[1]);

    auto make_action = [&](XrActionType t, const char* name, const char* local,
                           XrAction& out, bool per_hand) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        std::strncpy(aci.actionName, name, sizeof(aci.actionName) - 1);
        std::strncpy(aci.localizedActionName, local,
                     sizeof(aci.localizedActionName) - 1);
        aci.actionType = t;
        if (per_hand) {
            aci.countSubactionPaths = 2;
            aci.subactionPaths = hand_paths_;
        }
        xr_ok(xrCreateAction(action_set_, &aci, &out), "xrCreateAction");
    };

    make_action(XR_ACTION_TYPE_POSE_INPUT, "hand_pose", "Hand Pose", hand_pose_action_, true);
    make_action(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose", aim_pose_action_, true);
    make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", trigger_action_, true);
    make_action(XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", grip_action_, true);
    make_action(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick",
                thumbstick_action_, true);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a", "Button A", button_a_action_, false);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b", "Button B", button_b_action_, false);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x", "Button X", button_x_action_, false);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y", "Button Y", button_y_action_, false);
    // The left menu button is the one an app may use (the right one belongs
    // to the system): the conventional pause control.
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_menu", "Menu Button",
                button_menu_action_, false);
    // Capacitive touch ("capsense"): the controller reports a finger RESTING
    // on a control before it is pressed. This is what lets a virtual hand put
    // its index finger on the trigger and its thumb down on the rest, instead
    // of snapping between a flat palm and a fist.
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_touch", "Trigger Touch",
                trigger_touch_action_, true);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbrest_touch",
                "Thumbrest Touch", thumbrest_touch_action_, true);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_touch",
                "Thumbstick Touch", thumbstick_touch_action_, true);
    // Pressing the stick in: the conventional sprint control (hold the
    // left stick down while pushing it forward).
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click",
                "Thumbstick Click", thumbstick_click_action_, true);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_a_touch",
                "Button A Touch", button_a_touch_action_, false);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_b_touch",
                "Button B Touch", button_b_touch_action_, false);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_x_touch",
                "Button X Touch", button_x_touch_action_, false);
    make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "button_y_touch",
                "Button Y Touch", button_y_touch_action_, false);
    // Haptics are an OUTPUT action. Core OpenXR -- no extension required.
    make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic Feedback",
                haptic_action_, true);

    auto path = [&](const char* s) {
        XrPath p;
        xrStringToPath(xr_instance_, s, &p);
        return p;
    };
    std::vector<XrActionSuggestedBinding> sug{
        {hand_pose_action_, path("/user/hand/left/input/grip/pose")},
        {hand_pose_action_, path("/user/hand/right/input/grip/pose")},
        {aim_pose_action_,  path("/user/hand/left/input/aim/pose")},
        {aim_pose_action_,  path("/user/hand/right/input/aim/pose")},
        {trigger_action_, path("/user/hand/left/input/trigger/value")},
        {trigger_action_, path("/user/hand/right/input/trigger/value")},
        {grip_action_, path("/user/hand/left/input/squeeze/value")},
        {grip_action_, path("/user/hand/right/input/squeeze/value")},
        {thumbstick_action_, path("/user/hand/left/input/thumbstick")},
        {thumbstick_action_, path("/user/hand/right/input/thumbstick")},
        {button_a_action_, path("/user/hand/right/input/a/click")},
        {button_b_action_, path("/user/hand/right/input/b/click")},
        // X / Y live on the LEFT Touch controller only.
        {button_x_action_, path("/user/hand/left/input/x/click")},
        {button_y_action_, path("/user/hand/left/input/y/click")},
        {button_menu_action_, path("/user/hand/left/input/menu/click")},
        {trigger_touch_action_, path("/user/hand/left/input/trigger/touch")},
        {trigger_touch_action_, path("/user/hand/right/input/trigger/touch")},
        {thumbrest_touch_action_,
         path("/user/hand/left/input/thumbrest/touch")},
        {thumbrest_touch_action_,
         path("/user/hand/right/input/thumbrest/touch")},
        {thumbstick_touch_action_,
         path("/user/hand/left/input/thumbstick/touch")},
        {thumbstick_touch_action_,
         path("/user/hand/right/input/thumbstick/touch")},
        {thumbstick_click_action_,
         path("/user/hand/left/input/thumbstick/click")},
        {thumbstick_click_action_,
         path("/user/hand/right/input/thumbstick/click")},
        {button_a_touch_action_, path("/user/hand/right/input/a/touch")},
        {button_b_touch_action_, path("/user/hand/right/input/b/touch")},
        {button_x_touch_action_, path("/user/hand/left/input/x/touch")},
        {button_y_touch_action_, path("/user/hand/left/input/y/touch")},
        {haptic_action_, path("/user/hand/left/output/haptic")},
        {haptic_action_, path("/user/hand/right/output/haptic")},
    };

    // Suggest the SAME binding set against every Touch profile we support.
    // ``oculus/touch_controller`` is the classic profile every Meta runtime
    // exposes; ``meta/touch_controller_plus`` is the Quest 3 controller. A
    // runtime that does not know a profile rejects it with
    // XR_ERROR_PATH_UNSUPPORTED, which is expected rather than an error --
    // the other profile still carries the bindings.
    auto suggest = [&](const char* profile, bool required) {
        XrPath profile_path;
        if (XR_FAILED(xrStringToPath(xr_instance_, profile, &profile_path))) {
            if (required) {
                PTV_ERROR("XR: cannot resolve interaction profile {}", profile);
            }
            return;
        }
        XrInteractionProfileSuggestedBinding ipb{
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        ipb.interactionProfile = profile_path;
        ipb.suggestedBindings = sug.data();
        ipb.countSuggestedBindings = static_cast<uint32_t>(sug.size());
        const XrResult r =
            xrSuggestInteractionProfileBindings(xr_instance_, &ipb);
        if (XR_SUCCEEDED(r)) {
            PTV_INFO("XR: bound {} actions to {}", sug.size(), profile);
        } else if (required) {
            xr_ok(r, "xrSuggestInteractionProfileBindings");
        } else {
            PTV_INFO("XR: runtime does not support {} (optional, skipped)",
                     profile);
        }
    };
    suggest("/interaction_profiles/oculus/touch_controller", true);
    suggest("/interaction_profiles/meta/touch_controller_plus", false);

    // Hand (grip) pose spaces.
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo spi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spi.action = hand_pose_action_;
        spi.subactionPath = hand_paths_[i];
        spi.poseInActionSpace.orientation.w = 1.0f;
        xr_ok(xrCreateActionSpace(xr_session_, &spi, &hand_space_[i]),
              "xrCreateActionSpace(grip)");
    }
    // Aim pose spaces -- separate from grip because the runtime applies
    // a different (controller-body-aligned) rotation.
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo spi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spi.action = aim_pose_action_;
        spi.subactionPath = hand_paths_[i];
        spi.poseInActionSpace.orientation.w = 1.0f;
        xr_ok(xrCreateActionSpace(xr_session_, &spi, &aim_space_[i]),
              "xrCreateActionSpace(aim)");
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &action_set_;
    return xr_ok(xrAttachSessionActionSets(xr_session_, &attach),
                 "xrAttachSessionActionSets");
}

bool XrRuntime::create_swapchains() {
    uint32_t fmt_count = 0;
    xr_ok(xrEnumerateSwapchainFormats(xr_session_, 0, &fmt_count, nullptr),
          "xrEnumerateSwapchainFormats");
    std::vector<int64_t> fmts(fmt_count);
    xr_ok(xrEnumerateSwapchainFormats(xr_session_, fmt_count, &fmt_count, fmts.data()),
          "xrEnumerateSwapchainFormats#2");

    const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_FORMAT_R8G8B8A8_UNORM};
    int64_t chosen = fmts.empty() ? 0 : fmts[0];
    for (auto p : preferred) {
        if (std::find(fmts.begin(), fmts.end(), p) != fmts.end()) {
            chosen = p;
            break;
        }
    }
    color_format_ = static_cast<VkFormat>(chosen);

    // Allocated size: the recommended eye resolution times the ceiling the
    // scene asked for, within what the runtime allows. Dynamic resolution
    // then renders a centred sub-rectangle of this (set_render_scale).
    const float ceiling = std::clamp(swap_opts_.render_scale_ceiling, 0.5f, 1.25f);
    swap_width_ = std::min(
        view_config_views_[0].maxImageRectWidth,
        static_cast<uint32_t>(std::lround(
            static_cast<float>(view_config_views_[0].recommendedImageRectWidth) * ceiling)));
    swap_height_ = std::min(
        view_config_views_[0].maxImageRectHeight,
        static_cast<uint32_t>(std::lround(
            static_cast<float>(view_config_views_[0].recommendedImageRectHeight) * ceiling)));
    // Keep FFR tiles aligned.
    swap_width_ &= ~31u;
    swap_height_ &= ~31u;

    // Multiview: ONE swapchain whose images have two layers (left, right).
    const int chains = multiview_ ? 1 : 2;
    color_swapchains_.assign(static_cast<size_t>(chains), XR_NULL_HANDLE);
    color_images_.assign(static_cast<size_t>(chains), {});
    density_images_.assign(static_cast<size_t>(chains), {});
    for (int c = 0; c < chains; ++c) {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = chosen;
        sci.sampleCount = 1;
        sci.width = swap_width_;
        sci.height = swap_height_;
        sci.faceCount = 1;
        sci.arraySize = multiview_ ? 2u : 1u;
        sci.mipCount = 1;
        // Opt this swapchain in to foveation. Must be chained at CREATE time;
        // the level itself is set later (and can change per frame) through
        // xrUpdateSwapchainFB. With the FDM flag the runtime also gives each
        // image a fragment density map for our render pass.
        XrSwapchainCreateInfoFoveationFB foveation_ci{
            XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        foveation_ci.flags = fdm_ ? XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB : 0;
        if (foveation_supported_) {
            sci.next = &foveation_ci;
        }
        if (!xr_ok(xrCreateSwapchain(xr_session_, &sci, &color_swapchains_[c]),
                   "xrCreateSwapchain")) return false;

        uint32_t img_count = 0;
        xr_ok(xrEnumerateSwapchainImages(color_swapchains_[c], 0, &img_count, nullptr),
              "xrEnumerateSwapchainImages");
        color_images_[c].resize(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (fdm_) {
            density_images_[c].resize(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_FOVEATION_VULKAN_FB});
            for (uint32_t i = 0; i < img_count; ++i) {
                color_images_[c][i].next = &density_images_[c][i];
            }
        }
        xr_ok(xrEnumerateSwapchainImages(
                  color_swapchains_[c], img_count, &img_count,
                  reinterpret_cast<XrSwapchainImageBaseHeader*>(color_images_[c].data())),
              "xrEnumerateSwapchainImages#2");
        if (fdm_) {
            for (uint32_t i = 0; i < img_count; ++i) {
                color_images_[c][i].next = nullptr;
                if (density_images_[c][i].image == VK_NULL_HANDLE) {
                    PTV_WARN("XR: swapchain image {} has no density map; FFR via FDM off", i);
                    fdm_ = false;
                }
            }
            if (fdm_ && img_count > 0) {
                PTV_INFO("XR: fragment density maps {}x{} per image",
                         density_images_[c][0].width, density_images_[c][0].height);
            }
        }
    }
    return true;
}

void XrRuntime::set_foveation_level(int level) {
    if (!foveation_supported_ || xr_session_ == XR_NULL_HANDLE) return;
    level = std::max(0, std::min(level, 4));
    if (level == foveation_level_applied_) return;

    // Resolve the entry points once. These are extension functions, so they
    // are not in the loader's static export table.
    static PFN_xrCreateFoveationProfileFB create_profile = nullptr;
    static PFN_xrDestroyFoveationProfileFB destroy_profile = nullptr;
    static PFN_xrUpdateSwapchainFB update_swapchain = nullptr;
    if (create_profile == nullptr) {
        xrGetInstanceProcAddr(xr_instance_, "xrCreateFoveationProfileFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&create_profile));
        xrGetInstanceProcAddr(xr_instance_, "xrDestroyFoveationProfileFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&destroy_profile));
        xrGetInstanceProcAddr(xr_instance_, "xrUpdateSwapchainFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&update_swapchain));
    }
    if (!create_profile || !update_swapchain) {
        foveation_supported_ = false;
        return;
    }

    // Map our 0..4 scale onto the runtime's four levels. Meta's guidance is to
    // raise this for pixel-heavy scenes and drop it for menus/simple content,
    // which is exactly what the adaptive governor does with it.
    XrFoveationLevelFB xr_level = XR_FOVEATION_LEVEL_NONE_FB;
    switch (level) {
        case 1: xr_level = XR_FOVEATION_LEVEL_LOW_FB; break;
        case 2: xr_level = XR_FOVEATION_LEVEL_MEDIUM_FB; break;
        case 3:
        case 4: xr_level = XR_FOVEATION_LEVEL_HIGH_FB; break;
        default: xr_level = XR_FOVEATION_LEVEL_NONE_FB; break;
    }

    XrFoveationLevelProfileCreateInfoFB level_profile{
        XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    level_profile.level = xr_level;
    level_profile.verticalOffset = 0.0f;
    // The engine's governor owns the level (and honours user locks); the
    // runtime's own dynamic easing would move it underneath both.
    level_profile.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;

    XrFoveationProfileCreateInfoFB profile_ci{
        XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
    profile_ci.next = &level_profile;

    XrFoveationProfileFB profile = XR_NULL_HANDLE;
    if (create_profile(xr_session_, &profile_ci, &profile) != XR_SUCCESS) {
        PTV_WARN("XR: xrCreateFoveationProfileFB failed; disabling foveation");
        foveation_supported_ = false;
        return;
    }

    XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
    state.profile = profile;
    for (auto sc : color_swapchains_) {
        if (sc == XR_NULL_HANDLE) continue;
        update_swapchain(
            sc, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&state));
    }
    if (destroy_profile) destroy_profile(profile);

    foveation_level_applied_ = level;
    PTV_INFO("XR: foveation level -> {}", level);
}

bool XrRuntime::request_exit() {
    if (xr_session_ == XR_NULL_HANDLE || !is_running_) return false;
    const XrResult r = xrRequestExitSession(xr_session_);
    PTV_INFO("XR: exit requested (xrRequestExitSession -> {})", static_cast<int>(r));
    return XR_SUCCEEDED(r);
}

bool XrRuntime::poll_events() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(xr_instance_, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                session_state_ = e->state;
                if (e->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = view_config_;
                    xrBeginSession(xr_session_, &bi);
                    is_running_ = true;
                    PTV_INFO("XR session state: READY -> begin");
                } else if (e->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(xr_session_);
                    is_running_ = false;
                    PTV_INFO("XR session state: STOPPING -> end");
                } else if (e->state == XR_SESSION_STATE_EXITING ||
                           e->state == XR_SESSION_STATE_LOSS_PENDING) {
                    return false;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                return false;
            default:
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

bool XrRuntime::begin_frame(Views& views) {
    if (!is_running_) return false;

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    if (!xr_ok(xrWaitFrame(xr_session_, &fwi, &fs), "xrWaitFrame")) return false;

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xr_ok(xrBeginFrame(xr_session_, &fbi), "xrBeginFrame");

    last_predicted_display_time_ = fs.predictedDisplayTime;

    if (!fs.shouldRender) {
        end_frame_no_render();
        return false;
    }

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = view_config_;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = stage_space_;
    XrView xr_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t got = 0;
    xr_ok(xrLocateViews(xr_session_, &vli, &vs, 2, &got, xr_views),
          "xrLocateViews");
    views.left = xr_views[0];
    views.right = xr_views[1];
    views.predicted_display_time = fs.predictedDisplayTime;

    // For this skeleton we do not render: submit an empty composition so the
    // runtime keeps the session alive. Real rendering would acquire each eye
    // swapchain image, record commands, and build an XrCompositionLayerProjection.
    end_frame_no_render();
    return true;
}

void XrRuntime::end_frame_no_render() {
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = 0;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = 0;
    fei.layers = nullptr;
    xrEndFrame(xr_session_, &fei);
}

bool XrRuntime::begin_xr_frame() {
    if (!is_running_) return false;
    if (frame_started_) {
        // Defensive: a prior begin_xr_frame() didn't get paired. Treat
        // the existing state as still authoritative so we don't double
        // up xrWaitFrame (which would deadlock in some runtimes).
        return pending_frame_state_.shouldRender;
    }

    pending_frame_state_ = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    if (!xr_ok(xrWaitFrame(xr_session_, &fwi, &pending_frame_state_),
               "xrWaitFrame")) {
        return false;
    }

    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xr_ok(xrBeginFrame(xr_session_, &fbi), "xrBeginFrame");

    last_predicted_display_time_ = pending_frame_state_.predictedDisplayTime;
    frame_started_ = true;
    pending_views_valid_ = false;

    if (pending_frame_state_.shouldRender) {
        // Locate both eyes once, now: culling (before the draw list is built)
        // and rendering then use exactly the same poses.
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = view_config_;
        vli.displayTime = pending_frame_state_.predictedDisplayTime;
        vli.space = stage_space_;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t got = 0;
        pending_views_ = {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        if (XR_SUCCEEDED(xrLocateViews(xr_session_, &vli, &vs, 2, &got,
                                       pending_views_.data())) &&
            got == 2) {
            pending_views_valid_ =
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        }
    }

    if (!pending_frame_state_.shouldRender) {
        // Submit empty frame immediately so the runtime keeps the session
        // alive and the caller doesn't need a "skip render this frame"
        // branch.
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = pending_frame_state_.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 0;
        fei.layers = nullptr;
        xrEndFrame(xr_session_, &fei);
        frame_started_ = false;
        return false;
    }
    return true;
}

void XrRuntime::submit_pending_empty_frame() {
    if (!frame_started_) return;
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = pending_frame_state_.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = 0;
    fei.layers = nullptr;
    xrEndFrame(xr_session_, &fei);
    frame_started_ = false;
}

bool XrRuntime::render_frame(const RenderCallback& render) {
    if (!is_running_) return false;

    // ---- xrWaitFrame + xrBeginFrame ------------------------------------
    //
    // If begin_xr_frame() was already called this iteration (the
    // recommended path because it lets sync_input use a fresh
    // predicted display time), reuse the pending FrameState.
    // Otherwise fall back to the legacy single-call flow.
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const bool reuse_views = frame_started_ && pending_views_valid_;
    if (frame_started_) {
        fs = pending_frame_state_;
        frame_started_ = false;
    } else {
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        if (!xr_ok(xrWaitFrame(xr_session_, &fwi, &fs), "xrWaitFrame"))
            return false;

        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xr_ok(xrBeginFrame(xr_session_, &fbi), "xrBeginFrame");

        last_predicted_display_time_ = fs.predictedDisplayTime;
    }

    // If the runtime asks us not to render (e.g. app not focused), submit
    // an empty composition to keep the session alive and return.
    if (!fs.shouldRender) {
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 0;
        fei.layers = nullptr;
        xrEndFrame(xr_session_, &fei);
        return true;
    }

    // ---- Locate stereo views ------------------------------------------
    // Normally already located in begin_xr_frame (the same poses culling
    // used); located here only on the legacy single-call path.
    XrView xr_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool pose_valid = false;
    if (reuse_views) {
        xr_views[0] = pending_views_[0];
        xr_views[1] = pending_views_[1];
        pose_valid = true;
    } else {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = view_config_;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = stage_space_;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t got = 0;
        xr_ok(xrLocateViews(xr_session_, &vli, &vs, 2, &got, xr_views),
              "xrLocateViews");
        pose_valid = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                     (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    }
    pending_views_valid_ = false;

    // Dynamic resolution: a centred sub-rectangle, so the density map's
    // centre stays on the lens centre.
    const uint32_t rect_w = std::max<uint32_t>(
        64u, static_cast<uint32_t>(std::lround(static_cast<float>(swap_width_) * render_scale_)) &
                 ~7u);
    const uint32_t rect_h = std::max<uint32_t>(
        64u, static_cast<uint32_t>(std::lround(static_cast<float>(swap_height_) * render_scale_)) &
                 ~7u);
    const int32_t rect_x = static_cast<int32_t>((swap_width_ - rect_w) / 2u);
    const int32_t rect_y = static_cast<int32_t>((swap_height_ - rect_h) / 2u);

    // ---- Acquire swapchain images + invoke caller ---------------------
    const int chains = static_cast<int>(color_swapchains_.size());
    std::array<EyeImage, 2> eyes{};
    std::array<uint32_t, 2> acquired_indices{};
    bool acquired[2] = {false, false};
    for (int c = 0; c < chains; ++c) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!xr_ok(xrAcquireSwapchainImage(color_swapchains_[c], &ai, &idx),
                   "xrAcquireSwapchainImage")) {
            continue;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (!xr_ok(xrWaitSwapchainImage(color_swapchains_[c], &wi),
                   "xrWaitSwapchainImage")) {
            // Release to keep counts balanced.
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(color_swapchains_[c], &ri);
            continue;
        }
        acquired_indices[c] = idx;
        acquired[c] = true;
    }
    if (chains == 1) acquired[1] = acquired[0];
    for (int eye = 0; eye < 2; ++eye) {
        const int c = chains == 1 ? 0 : eye;
        if (!acquired[eye]) continue;
        const uint32_t idx = acquired_indices[c];
        eyes[eye].image = color_images_[c][idx].image;
        eyes[eye].array_layer = chains == 1 ? static_cast<uint32_t>(eye) : 0u;
        eyes[eye].offset_x = rect_x;
        eyes[eye].offset_y = rect_y;
        eyes[eye].width = rect_w;
        eyes[eye].height = rect_h;
        eyes[eye].fov = xr_views[eye].fov;
        eyes[eye].pose = xr_views[eye].pose;
        if (fdm_ && idx < density_images_[c].size()) {
            eyes[eye].density_map = density_images_[c][idx].image;
            eyes[eye].density_width = density_images_[c][idx].width;
            eyes[eye].density_height = density_images_[c][idx].height;
        }
    }

    if (pose_valid && acquired[0] && acquired[1] && render) {
        render(eyes, fs.predictedDisplayTime);
    }

    // ---- Release swapchain images -------------------------------------
    for (int c = 0; c < chains; ++c) {
        if (!acquired[c]) continue;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(color_swapchains_[c], &ri);
    }

    // ---- Build composition layer + xrEndFrame -------------------------
    std::array<XrCompositionLayerProjectionView, 2> proj_views{};
    for (int eye = 0; eye < 2; ++eye) {
        proj_views[eye] = XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        proj_views[eye].pose = xr_views[eye].pose;
        proj_views[eye].fov = xr_views[eye].fov;
        proj_views[eye].subImage.swapchain = color_swapchains_[chains == 1 ? 0 : eye];
        proj_views[eye].subImage.imageArrayIndex = chains == 1 ? static_cast<uint32_t>(eye) : 0u;
        proj_views[eye].subImage.imageRect.offset = {rect_x, rect_y};
        proj_views[eye].subImage.imageRect.extent = {static_cast<int32_t>(rect_w),
                                                     static_cast<int32_t>(rect_h)};
    }

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = stage_space_;
    layer.viewCount = 2;
    layer.views = proj_views.data();

    const XrCompositionLayerBaseHeader* layer_ptr =
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (pose_valid && acquired[0] && acquired[1]) ? 1 : 0;
    fei.layers = fei.layerCount ? &layer_ptr : nullptr;
    xr_ok(xrEndFrame(xr_session_, &fei), "xrEndFrame");
    return true;
}

void XrRuntime::sync_input(InputState& out) {
    if (!is_running_) return;
    XrActiveActionSet active{action_set_, XR_NULL_PATH};
    XrActionsSyncInfo asi{XR_TYPE_ACTIONS_SYNC_INFO};
    asi.countActiveActionSets = 1;
    asi.activeActionSets = &active;
    xrSyncActions(xr_session_, &asi);

    // ---- Hand poses (grip pose) in stage space ------------------------
    //
    // The spec requires checking the pose action's isActive bit before
    // locating its action space, and trusting only the position/orientation
    // VALID bits on the resulting XrSpaceLocation. Without this controllers
    // that are not yet tracked (e.g. still sleeping in their charging dock)
    // would silently report the origin.
    auto read_pose = [&](XrAction action, XrSpace space, int i,
                         XrSpaceLocation& loc, bool& valid) {
        loc = XrSpaceLocation{XR_TYPE_SPACE_LOCATION};
        loc.pose.orientation.w = 1.0f;
        valid = false;

        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        gi.subactionPath = hand_paths_[i];
        if (xrGetActionStatePose(xr_session_, &gi, &ps) != XR_SUCCESS ||
            !ps.isActive) {
            return;
        }
        if (last_predicted_display_time_ == 0) return;
        if (space == XR_NULL_HANDLE || stage_space_ == XR_NULL_HANDLE) return;

        XrResult r = xrLocateSpace(space, stage_space_,
                                   last_predicted_display_time_, &loc);
        if (!XR_SUCCEEDED(r)) return;
        const auto flags = loc.locationFlags;
        if ((flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
            (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            valid = true;
        }
    };
    read_pose(hand_pose_action_, hand_space_[0], 0,
              out.hand_left, out.hand_left_valid);
    read_pose(hand_pose_action_, hand_space_[1], 1,
              out.hand_right, out.hand_right_valid);
    read_pose(aim_pose_action_, aim_space_[0], 0,
              out.aim_left, out.aim_left_valid);
    read_pose(aim_pose_action_, aim_space_[1], 1,
              out.aim_right, out.aim_right_valid);

    // ---- HMD pose (VIEW reference space) in stage space ---------------
    //
    // Locating ``view_space_`` against ``stage_space_`` at the predicted
    // display time gives us a fresh head pose every frame, in the SAME
    // coordinate system as the hand poses above. This is what Meta's
    // and Unity XR's locomotion samples consume directly: head and hand
    // both expressed in the play-space stage, so there's no LOCAL/STAGE
    // /VIEW mixing for downstream gameplay code to get wrong.
    //
    // (The render path still calls xrLocateViews separately for the
    // per-eye projection poses; that's fine -- locomotion just needs a
    // single center-eye pose ASAP, before locomotion logic runs.)
    out.head = XrSpaceLocation{XR_TYPE_SPACE_LOCATION};
    out.head.pose.orientation.w = 1.0f;
    out.head_valid = false;
    if (view_space_ != XR_NULL_HANDLE && stage_space_ != XR_NULL_HANDLE &&
        last_predicted_display_time_ != 0) {
        XrSpaceLocation hloc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(view_space_, stage_space_,
                                       last_predicted_display_time_, &hloc))) {
            const auto flags = hloc.locationFlags;
            if ((flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                out.head = hloc;
                out.head_valid = true;
            }
        }
    }

    auto read_float = [&](XrAction a, XrPath sub, float& v) {
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        if (xrGetActionStateFloat(xr_session_, &gi, &st) == XR_SUCCESS &&
            st.isActive) {
            v = st.currentState;
        }
    };
    read_float(trigger_action_, hand_paths_[0], out.trigger[0]);
    read_float(trigger_action_, hand_paths_[1], out.trigger[1]);
    read_float(grip_action_, hand_paths_[0], out.grip[0]);
    read_float(grip_action_, hand_paths_[1], out.grip[1]);

    auto read_vec2 = [&](XrAction a, XrPath sub, XrVector2f& v) {
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        if (xrGetActionStateVector2f(xr_session_, &gi, &st) == XR_SUCCESS &&
            st.isActive) {
            v = st.currentState;
        }
    };
    read_vec2(thumbstick_action_, hand_paths_[0], out.thumbstick[0]);
    read_vec2(thumbstick_action_, hand_paths_[1], out.thumbstick[1]);

    // Face buttons. A/B are bound on the right controller, X/Y on the left.
    // These actions have no subaction paths, so we query with XR_NULL_PATH.
    auto read_bool = [&](XrAction a, bool& v) {
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        v = (xrGetActionStateBoolean(xr_session_, &gi, &st) == XR_SUCCESS &&
             st.isActive && st.currentState);
    };
    read_bool(button_a_action_, out.button_a);
    read_bool(button_b_action_, out.button_b);
    read_bool(button_x_action_, out.button_x);
    read_bool(button_y_action_, out.button_y);
    read_bool(button_menu_action_, out.button_menu);
    read_bool(button_a_touch_action_, out.button_a_touch);
    read_bool(button_b_touch_action_, out.button_b_touch);
    read_bool(button_x_touch_action_, out.button_x_touch);
    read_bool(button_y_touch_action_, out.button_y_touch);

    // Capsense. Per-hand, so unlike the face buttons these need the subaction
    // path. An inactive action (older controller, or a runtime that did not
    // take the capsense bindings) simply leaves the flag false, which reads as
    // "finger not resting there" -- the correct degradation.
    auto read_bool_hand = [&](XrAction a, XrPath sub, bool& v) {
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = sub;
        v = (xrGetActionStateBoolean(xr_session_, &gi, &st) == XR_SUCCESS &&
             st.isActive && st.currentState);
    };
    for (int i = 0; i < 2; ++i) {
        read_bool_hand(trigger_touch_action_, hand_paths_[i],
                       out.trigger_touch[i]);
        read_bool_hand(thumbrest_touch_action_, hand_paths_[i],
                       out.thumbrest_touch[i]);
        read_bool_hand(thumbstick_touch_action_, hand_paths_[i],
                       out.thumbstick_touch[i]);
        read_bool_hand(thumbstick_click_action_, hand_paths_[i],
                       out.thumbstick_click[i]);
    }
}

void XrRuntime::apply_haptic(int hand, float duration_s, float frequency_hz,
                             float amplitude) {
    if (!is_running_ || haptic_action_ == XR_NULL_HANDLE) return;
    if (hand < 0 || hand > 1) return;
    if (!std::isfinite(duration_s) || !std::isfinite(frequency_hz) ||
        !std::isfinite(amplitude)) {
        return;
    }
    if (amplitude <= 0.0f || duration_s <= 0.0f) return;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.duration =
        static_cast<XrDuration>(static_cast<double>(duration_s) * 1e9);
    vibration.frequency = frequency_hz;
    vibration.amplitude = std::min(1.0f, amplitude);

    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = haptic_action_;
    info.subactionPath = hand_paths_[hand];
    xrApplyHapticFeedback(xr_session_, &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

void XrRuntime::shutdown() {
    for (auto sc : color_swapchains_)
        if (sc) xrDestroySwapchain(sc);
    color_swapchains_.clear();
    color_images_.clear();
    for (int i = 0; i < 2; ++i) {
        if (hand_space_[i]) xrDestroySpace(hand_space_[i]);
        if (aim_space_[i]) xrDestroySpace(aim_space_[i]);
    }
    if (view_space_) xrDestroySpace(view_space_);
    if (stage_space_) xrDestroySpace(stage_space_);
    if (action_set_) xrDestroyActionSet(action_set_);
    if (xr_session_) xrDestroySession(xr_session_);
    if (xr_instance_) xrDestroyInstance(xr_instance_);
    stage_space_ = XR_NULL_HANDLE;
    view_space_ = XR_NULL_HANDLE;
    hand_space_[0] = hand_space_[1] = XR_NULL_HANDLE;
    aim_space_[0] = aim_space_[1] = XR_NULL_HANDLE;
    action_set_ = XR_NULL_HANDLE;
    xr_session_ = XR_NULL_HANDLE;
    xr_instance_ = XR_NULL_HANDLE;
    frame_started_ = false;
    last_predicted_display_time_ = 0;
}

}  // namespace ptv
