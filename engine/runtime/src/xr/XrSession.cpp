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

    PTV_INFO("XR: creating swapchains...");
    if (!create_swapchains()) return false;
    PTV_INFO("XR: swapchains ok");

    PTV_INFO("OpenXR runtime ready ({}x{} per eye)",
             view_config_views_[0].recommendedImageRectWidth,
             view_config_views_[0].recommendedImageRectHeight);
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
    }
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

    color_swapchains_.resize(2);
    color_images_.resize(2);
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = chosen;
        sci.sampleCount = 1;
        sci.width = view_config_views_[eye].recommendedImageRectWidth;
        sci.height = view_config_views_[eye].recommendedImageRectHeight;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        // Opt this swapchain in to foveation. Must be chained at CREATE time;
        // the level itself is set later (and can change per frame) through
        // xrUpdateSwapchainFB.
        XrSwapchainCreateInfoFoveationFB foveation_ci{
            XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        if (foveation_supported_) {
            sci.next = &foveation_ci;
        }
        if (!xr_ok(xrCreateSwapchain(xr_session_, &sci, &color_swapchains_[eye]),
                   "xrCreateSwapchain")) return false;

        uint32_t img_count = 0;
        xr_ok(xrEnumerateSwapchainImages(color_swapchains_[eye], 0, &img_count, nullptr),
              "xrEnumerateSwapchainImages");
        color_images_[eye].resize(img_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        xr_ok(xrEnumerateSwapchainImages(
                  color_swapchains_[eye], img_count, &img_count,
                  reinterpret_cast<XrSwapchainImageBaseHeader*>(color_images_[eye].data())),
              "xrEnumerateSwapchainImages#2");
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
    // Dynamic lets the runtime ease the level with GPU load; at level 4 we ask
    // for the strongest static setting instead.
    level_profile.dynamic = (level >= 4) ? XR_FOVEATION_DYNAMIC_DISABLED_FB
                                         : XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB;

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
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = view_config_;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = stage_space_;
    XrView xr_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t got = 0;
    xr_ok(xrLocateViews(xr_session_, &vli, &vs, 2, &got, xr_views),
          "xrLocateViews");

    const bool pose_valid =
        (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

    // ---- Acquire swapchain images + invoke caller ---------------------
    std::array<EyeImage, 2> eyes{};
    std::array<uint32_t, 2> acquired_indices{};
    bool acquired[2] = {false, false};
    for (int eye = 0; eye < 2; ++eye) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!xr_ok(xrAcquireSwapchainImage(color_swapchains_[eye], &ai, &idx),
                   "xrAcquireSwapchainImage")) {
            continue;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (!xr_ok(xrWaitSwapchainImage(color_swapchains_[eye], &wi),
                   "xrWaitSwapchainImage")) {
            // Release to keep counts balanced.
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(color_swapchains_[eye], &ri);
            continue;
        }
        acquired_indices[eye] = idx;
        acquired[eye] = true;
        eyes[eye].image = color_images_[eye][idx].image;
        eyes[eye].width = view_config_views_[eye].recommendedImageRectWidth;
        eyes[eye].height = view_config_views_[eye].recommendedImageRectHeight;
        eyes[eye].fov = xr_views[eye].fov;
        eyes[eye].pose = xr_views[eye].pose;
    }

    if (pose_valid && acquired[0] && acquired[1] && render) {
        render(eyes, fs.predictedDisplayTime);
    }

    // ---- Release swapchain images -------------------------------------
    for (int eye = 0; eye < 2; ++eye) {
        if (!acquired[eye]) continue;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(color_swapchains_[eye], &ri);
    }

    // ---- Build composition layer + xrEndFrame -------------------------
    std::array<XrCompositionLayerProjectionView, 2> proj_views{};
    for (int eye = 0; eye < 2; ++eye) {
        proj_views[eye] = XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        proj_views[eye].pose = xr_views[eye].pose;
        proj_views[eye].fov = xr_views[eye].fov;
        proj_views[eye].subImage.swapchain = color_swapchains_[eye];
        proj_views[eye].subImage.imageArrayIndex = 0;
        proj_views[eye].subImage.imageRect.offset = {0, 0};
        proj_views[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(
                view_config_views_[eye].recommendedImageRectWidth),
            static_cast<int32_t>(
                view_config_views_[eye].recommendedImageRectHeight),
        };
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
