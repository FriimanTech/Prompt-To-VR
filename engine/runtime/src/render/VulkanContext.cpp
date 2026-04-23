#define VOLK_IMPLEMENTATION
#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

// OpenXR headers pulled in only here to keep VulkanContext.h light. We need
// the platform macros in the same order XrSession.h establishes them.
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
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

#include "ptv/render/VulkanContext.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "ptv/core/Logger.h"

namespace ptv {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        PTV_ERROR("[vk] {}", data->pMessage ? data->pMessage : "");
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        PTV_WARN("[vk] {}", data->pMessage ? data->pMessage : "");
    } else {
        PTV_DEBUG("[vk] {}", data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool ext_supported(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (auto& p : props)
        if (std::strcmp(p.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

VulkanContext::~VulkanContext() { shutdown(); }

bool VulkanContext::initialize(const Config& cfg) {
    if (volkInitialize() != VK_SUCCESS) {
        PTV_ERROR("volkInitialize failed. Is the Vulkan SDK installed?");
        return false;
    }

    uint32_t api_version = VK_API_VERSION_1_3;

    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = cfg.app_name.c_str();
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ptv_engine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = api_version;

    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> avail(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());

    std::vector<const char*> exts;
    for (auto& s : cfg.required_instance_extensions) exts.push_back(s.c_str());
    if (cfg.enable_validation &&
        ext_supported(avail, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        validation_ = true;
    }

    std::vector<const char*> layers;
    if (cfg.enable_validation) {
        uint32_t lc = 0;
        vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> lavail(lc);
        vkEnumerateInstanceLayerProperties(&lc, lavail.data());
        for (auto& l : lavail) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                break;
            }
        }
    }

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
        PTV_ERROR("vkCreateInstance failed");
        return false;
    }
    volkLoadInstance(instance_);

    if (validation_) {
        VkDebugUtilsMessengerCreateInfoEXT dci{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debug_cb;
        if (vkCreateDebugUtilsMessengerEXT) {
            vkCreateDebugUtilsMessengerEXT(instance_, &dci, nullptr, &messenger_);
        }
    }

    if (!pick_physical_device(cfg.required_device_extensions)) return false;
    if (!create_logical_device(cfg.required_device_extensions)) return false;
    if (!create_allocator()) return false;

    PTV_INFO("Vulkan context ready (validation={})", validation_);
    return true;
}

bool VulkanContext::pick_physical_device(
    const std::vector<std::string>& required_device_extensions) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (n == 0) {
        PTV_ERROR("No Vulkan physical devices found.");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());

    for (auto d : devs) {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qp(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, qp.data());
        for (uint32_t i = 0; i < qc; ++i) {
            if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical_device_ = d;
                graphics_queue_family_ = i;
                break;
            }
        }
        if (physical_device_) break;
    }
    if (!physical_device_) {
        PTV_ERROR("No physical device supports a graphics queue.");
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    PTV_INFO("GPU: {}", props.deviceName);

    (void)required_device_extensions;
    return true;
}

bool VulkanContext::create_logical_device(
    const std::vector<std::string>& required_device_extensions) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphics_queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    std::vector<const char*> exts;
    for (auto& s : required_device_extensions) exts.push_back(s.c_str());
    // Swapchain only required if we ever present directly; OpenXR owns the HMD
    // swapchain, so we do NOT add VK_KHR_SWAPCHAIN here automatically.

    // Query what the physical device actually supports so we can safely
    // request the features Meta Quest Link's OpenXR compositor shaders need
    // without risking vkCreateDevice() failing on older GPUs.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_device_, &supported);

    VkPhysicalDeviceFeatures feats{};
    feats.fillModeNonSolid = supported.fillModeNonSolid;
    feats.samplerAnisotropy = supported.samplerAnisotropy;
    // These are required by the Oculus / Meta Quest Link OpenXR runtime's
    // compositor / distortion shaders. Without them the validation layer
    // raises "SPIR-V Capability StorageImageMultisample was declared..."
    // and similar during xrCreateSession.
    feats.shaderStorageImageMultisample = supported.shaderStorageImageMultisample;
    feats.shaderStorageImageReadWithoutFormat =
        supported.shaderStorageImageReadWithoutFormat;
    feats.shaderStorageImageWriteWithoutFormat =
        supported.shaderStorageImageWriteWithoutFormat;
    feats.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    feats.imageCubeArray = supported.imageCubeArray;
    feats.independentBlend = supported.independentBlend;
    feats.multiDrawIndirect = supported.multiDrawIndirect;
    feats.shaderSampledImageArrayDynamicIndexing =
        supported.shaderSampledImageArrayDynamicIndexing;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    dci.pEnabledFeatures = &feats;

    if (vkCreateDevice(physical_device_, &dci, nullptr, &device_) != VK_SUCCESS) {
        PTV_ERROR("vkCreateDevice failed");
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);
    return true;
}

bool VulkanContext::create_allocator() {
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.physicalDevice = physical_device_;
    ci.device = device_;
    ci.instance = instance_;
    ci.pVulkanFunctions = &fns;
    if (vmaCreateAllocator(&ci, &allocator_) != VK_SUCCESS) {
        PTV_ERROR("vmaCreateAllocator failed");
        return false;
    }
    return true;
}

void VulkanContext::shutdown() {
    if (allocator_) {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }
    if (device_) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (messenger_ && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// OpenXR-aware init path (XR_KHR_vulkan_enable2)
// ---------------------------------------------------------------------------

namespace {

template <typename Fn>
Fn xr_resolve(XrInstance inst, const char* name) {
    Fn fn = nullptr;
    xrGetInstanceProcAddr(inst, name,
                          reinterpret_cast<PFN_xrVoidFunction*>(&fn));
    return fn;
}

}  // namespace

bool VulkanContext::initialize_for_openxr(const Config& cfg,
                                          XrInstance xr_instance,
                                          XrSystemId xr_system,
                                          uint32_t runtime_min_vulkan_api,
                                          uint32_t runtime_max_vulkan_api) {
    (void)runtime_max_vulkan_api;

    if (volkInitialize() != VK_SUCCESS) {
        PTV_ERROR("volkInitialize failed. Is the Vulkan SDK installed?");
        return false;
    }

    // Resolve the KHR helpers. These are required when XR_KHR_vulkan_enable2
    // is enabled on the XR instance.
    auto pfn_create_vk_instance =
        xr_resolve<PFN_xrCreateVulkanInstanceKHR>(xr_instance,
                                                  "xrCreateVulkanInstanceKHR");
    auto pfn_get_vk_device =
        xr_resolve<PFN_xrGetVulkanGraphicsDevice2KHR>(xr_instance,
                                                     "xrGetVulkanGraphicsDevice2KHR");
    auto pfn_create_vk_device =
        xr_resolve<PFN_xrCreateVulkanDeviceKHR>(xr_instance,
                                                "xrCreateVulkanDeviceKHR");
    if (!pfn_create_vk_instance || !pfn_get_vk_device || !pfn_create_vk_device) {
        PTV_ERROR("XR_KHR_vulkan_enable2 entry points not resolvable.");
        return false;
    }

    // Clamp our instance's apiVersion to something the runtime accepts.
    // Meta Quest Link commonly reports min=1.0 and max somewhere in 1.0..1.3.
    uint32_t api_version = VK_API_VERSION_1_1;
    if (runtime_min_vulkan_api != 0) {
        // runtime_min_vulkan_api is encoded with VK_MAKE_API_VERSION by the
        // OpenXR spec (packed into the low 32 bits of the XrVersion).
        uint32_t low = static_cast<uint32_t>(runtime_min_vulkan_api);
        if (low >= VK_API_VERSION_1_1) api_version = low;
    }
    if (cfg.api_version_cap != 0 && cfg.api_version_cap < api_version) {
        api_version = cfg.api_version_cap;
    }
    PTV_INFO("VK: instance apiVersion selected = {}.{}",
             VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version));

    // ---- Build Vulkan instance create info -----------------------------
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = cfg.app_name.c_str();
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "ptv_engine";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = api_version;

    std::vector<const char*> inst_exts;
    for (auto& s : cfg.required_instance_extensions) inst_exts.push_back(s.c_str());

    std::vector<const char*> layers;
    if (cfg.enable_validation) {
        uint32_t lc = 0;
        vkEnumerateInstanceLayerProperties(&lc, nullptr);
        std::vector<VkLayerProperties> lavail(lc);
        vkEnumerateInstanceLayerProperties(&lc, lavail.data());
        for (auto& l : lavail) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                break;
            }
        }
    }
    if (cfg.enable_validation) {
        inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app_info;
    ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.empty() ? nullptr : inst_exts.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    XrVulkanInstanceCreateInfoKHR xr_ici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xr_ici.systemId = xr_system;
    xr_ici.pfnGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkGetInstanceProcAddr);
    xr_ici.vulkanCreateInfo = &ici;
    xr_ici.vulkanAllocator = nullptr;
    xr_ici.createFlags = 0;

    VkResult vk_rc = VK_SUCCESS;
    XrResult xr_rc =
        pfn_create_vk_instance(xr_instance, &xr_ici, &instance_, &vk_rc);
    if (XR_FAILED(xr_rc) || vk_rc != VK_SUCCESS) {
        PTV_ERROR("xrCreateVulkanInstanceKHR failed: xr_rc={} vk_rc={}",
                  static_cast<int>(xr_rc), static_cast<int>(vk_rc));
        return false;
    }
    volkLoadInstance(instance_);
    validation_ = !layers.empty();

    if (validation_ && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dci{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debug_cb;
        vkCreateDebugUtilsMessengerEXT(instance_, &dci, nullptr, &messenger_);
    }

    // ---- Pick physical device chosen by the XR runtime -----------------
    XrVulkanGraphicsDeviceGetInfoKHR dgi{
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    dgi.systemId = xr_system;
    dgi.vulkanInstance = instance_;
    if (XR_FAILED(pfn_get_vk_device(xr_instance, &dgi, &physical_device_))) {
        PTV_ERROR("xrGetVulkanGraphicsDevice2KHR failed.");
        return false;
    }

    // Pick a graphics queue family on that device.
    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qc, qp.data());
    graphics_queue_family_ = 0;
    for (uint32_t i = 0; i < qc; ++i) {
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_ = i;
            break;
        }
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_device_, &props);
    PTV_INFO("VK: XR-selected GPU: {}", props.deviceName);

    // ---- Build device create info, let OpenXR add its extensions ------
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphics_queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    std::vector<const char*> dev_exts;
    for (auto& s : cfg.required_device_extensions) dev_exts.push_back(s.c_str());

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_device_, &supported);

    VkPhysicalDeviceFeatures feats{};
    feats.fillModeNonSolid = supported.fillModeNonSolid;
    feats.samplerAnisotropy = supported.samplerAnisotropy;
    feats.shaderStorageImageMultisample = supported.shaderStorageImageMultisample;
    feats.shaderStorageImageReadWithoutFormat =
        supported.shaderStorageImageReadWithoutFormat;
    feats.shaderStorageImageWriteWithoutFormat =
        supported.shaderStorageImageWriteWithoutFormat;
    feats.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    feats.imageCubeArray = supported.imageCubeArray;
    feats.independentBlend = supported.independentBlend;
    feats.multiDrawIndirect = supported.multiDrawIndirect;
    feats.shaderSampledImageArrayDynamicIndexing =
        supported.shaderSampledImageArrayDynamicIndexing;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
    dci.pEnabledFeatures = &feats;

    XrVulkanDeviceCreateInfoKHR xr_dci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_dci.systemId = xr_system;
    xr_dci.pfnGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkGetInstanceProcAddr);
    xr_dci.vulkanPhysicalDevice = physical_device_;
    xr_dci.vulkanCreateInfo = &dci;
    xr_dci.vulkanAllocator = nullptr;
    xr_dci.createFlags = 0;

    if (XR_FAILED(pfn_create_vk_device(xr_instance, &xr_dci, &device_, &vk_rc)) ||
        vk_rc != VK_SUCCESS) {
        PTV_ERROR("xrCreateVulkanDeviceKHR failed: vk_rc={}",
                  static_cast<int>(vk_rc));
        return false;
    }
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, graphics_queue_family_, 0, &graphics_queue_);

    // VMA allocator
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.vulkanApiVersion = api_version;
    aci.physicalDevice = physical_device_;
    aci.device = device_;
    aci.instance = instance_;
    aci.pVulkanFunctions = &fns;
    if (vmaCreateAllocator(&aci, &allocator_) != VK_SUCCESS) {
        PTV_ERROR("vmaCreateAllocator failed");
        return false;
    }

    PTV_INFO("Vulkan (via OpenXR helpers) ready (validation={})", validation_);
    return true;
}

}  // namespace ptv
