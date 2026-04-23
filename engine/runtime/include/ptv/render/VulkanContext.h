#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Forward-declare the OpenXR handles we need for the XR-aware init path.
// We do not include <openxr/openxr.h> here to keep this header light.
typedef struct XrInstance_T* XrInstance;
typedef uint64_t XrSystemId;

namespace ptv {

/**
 * Minimal Vulkan context suitable for OpenXR + Vulkan interop.
 *
 * Creating surfaces and swapchains for the HMD is the responsibility of the
 * XrSession; this context only provides the instance / device / queue /
 * allocator that the XR session and the renderer share.
 *
 * For OpenXR apps use :meth:`initialize_for_openxr` which delegates instance
 * and device creation to ``xrCreateVulkanInstanceKHR`` /
 * ``xrCreateVulkanDeviceKHR`` so the runtime can inject the extensions it
 * needs (external memory/semaphore/fence win32, dedicated allocation, etc.).
 * The plain :meth:`initialize` path is kept for headless / non-XR tools.
 */
class VulkanContext {
public:
    struct Config {
        std::string app_name{"ptv_runtime"};
        std::vector<std::string> required_instance_extensions{};
        std::vector<std::string> required_device_extensions{};
        bool enable_validation{true};
        /** Cap the Vulkan instance API version. 0 = library default. */
        uint32_t api_version_cap{0};
    };

    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /** Classic path: create VkInstance/VkDevice directly via Vulkan. */
    bool initialize(const Config& cfg);

    /** OpenXR-aware path: delegate instance/device creation to OpenXR so the
     *  runtime's required extensions are guaranteed to be present. Must be
     *  called AFTER the XR instance + system are ready. */
    bool initialize_for_openxr(const Config& cfg,
                               XrInstance xr_instance,
                               XrSystemId xr_system,
                               uint32_t runtime_min_vulkan_api,
                               uint32_t runtime_max_vulkan_api);

    void shutdown();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice device() const { return device_; }
    uint32_t graphics_queue_family() const { return graphics_queue_family_; }
    VkQueue graphics_queue() const { return graphics_queue_; }
    VmaAllocator allocator() const { return allocator_; }

private:
    bool pick_physical_device(const std::vector<std::string>& required_device_extensions);
    bool create_logical_device(const std::vector<std::string>& required_device_extensions);
    bool create_allocator();

    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue graphics_queue_{VK_NULL_HANDLE};
    uint32_t graphics_queue_family_{0};
    VmaAllocator allocator_{nullptr};
    bool validation_{false};
};

}  // namespace ptv
