#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv {

class VulkanContext;
class World;
class XrRuntime;

/**
 * Minimal forward renderer for the VR stage.
 *
 * The renderer owns:
 *   * a color + depth render pass,
 *   * a graphics pipeline (push-constant driven),
 *   * a unit cube vertex / index buffer,
 *   * a per-eye depth image,
 *   * a framebuffer cache keyed by VkImage (we build a framebuffer the first
 *     time we see an XR swapchain image).
 *
 * It does not own the swapchains themselves; those are owned by
 * :class:`XrRuntime`. The main loop calls :meth:`draw_frame` once per XR
 * frame, passing in the two eye views and the acquired swapchain image
 * indices.
 */
class Renderer {
public:
    struct DrawItem {
        Mat4 model{1.0f};
        Vec4 color{0.8f, 0.8f, 0.82f, 1.0f};
        // Per-item shader selector:
        //   0 = default lit object
        //   1 = floor (adds a procedural grid / checker in the fragment
        //       shader; the X/Z world coordinates drive the pattern)
        //   2 = hand / grab highlight (slightly emissive rim)
        float material_id{0.0f};
        // -1 = draw the built-in unit cube (legacy procedural entities).
        // >=0 = index into this renderer's uploaded GLB mesh cache.
        int mesh_slot{-1};
    };

    struct EyeTarget {
        VkImage image{VK_NULL_HANDLE};
        int32_t offset_x{0};
        int32_t offset_y{0};
        uint32_t width{0};
        uint32_t height{0};
        Mat4 view{1.0f};
        Mat4 projection{1.0f};
    };

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(VulkanContext& vk,
                    VkFormat color_format,
                    uint32_t eye_width,
                    uint32_t eye_height);
    void shutdown();

    /** Render the two eyes. The caller has already acquired+waited each
     *  swapchain image; the renderer will submit the command buffer and
     *  wait for the GPU on the main thread (simple, low-throughput). */
    void draw_frame(const std::array<EyeTarget, 2>& eyes,
                    const std::vector<DrawItem>& items,
                    const Vec3& sky_color);

    /** Build a list of DrawItems from the ECS. Entities with ``CMesh`` load
     *  GLB geometry on first use; others use the procedural unit cube. */
    void build_items_from_world(World& world, std::vector<DrawItem>& out);

    /** Pre-load (or fetch from cache) a GLB mesh for use as a UI icon /
     *  manually-placed DrawItem. Returns the cache slot to set on
     *  ``DrawItem::mesh_slot``, or -1 on failure (file not found, no
     *  triangles, etc.). The mesh is rescaled so its longest axis is
     *  ``target_scale_meters`` -- see GltfMeshLoad. Safe to call multiple
     *  times with the same path; the second call returns the cached slot. */
    int load_glb(const std::string& resolved_path, float target_scale_meters);

    VkFormat color_format() const { return color_format_; }
    VkRenderPass render_pass() const { return render_pass_; }

private:
    bool create_render_pass();
    bool create_pipeline();
    bool create_cube_mesh();
    bool create_command_resources();
    bool create_depth_image();

    VkImageView get_or_create_color_view(VkImage img);
    VkFramebuffer get_or_create_framebuffer(VkImage img);

    struct GltfGpuMesh {
        VkBuffer vb{VK_NULL_HANDLE};
        VmaAllocation vb_alloc{nullptr};
        VkBuffer ib{VK_NULL_HANDLE};
        VmaAllocation ib_alloc{nullptr};
        uint32_t index_count{0};
    };
    /** Return cache slot for ``resolved_path`` (absolute on disk), or -1. */
    int ensure_gltf_mesh(const std::string& resolved_path,
                         float target_scale_meters);
    /** Upload (or fetch cached) an arbitrary procedural triangle mesh authored
     *  by the world-generation stage. ``key`` is a stable per-entity id used
     *  to cache the GPU buffers so we upload once, not every frame. ``normals``
     *  may be empty (flat normals are computed). Returns a mesh cache slot or
     *  -1. */
    int ensure_inline_mesh(std::uint32_t key,
                           const std::vector<float>& positions,
                           const std::vector<float>& normals,
                           const std::vector<std::uint32_t>& indices,
                           const Vec3& color);
    void destroy_gltf_meshes();

    VulkanContext* vk_{nullptr};
    VkFormat color_format_{VK_FORMAT_UNDEFINED};
    VkFormat depth_format_{VK_FORMAT_D32_SFLOAT};
    uint32_t eye_width_{0};
    uint32_t eye_height_{0};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    VkShaderModule vs_{VK_NULL_HANDLE};
    VkShaderModule fs_{VK_NULL_HANDLE};

    VkBuffer cube_vb_{VK_NULL_HANDLE};
    VmaAllocation cube_vb_alloc_{nullptr};
    VkBuffer cube_ib_{VK_NULL_HANDLE};
    VmaAllocation cube_ib_alloc_{nullptr};
    uint32_t cube_index_count_{0};

    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_alloc_{nullptr};
    VkImageView depth_view_{VK_NULL_HANDLE};

    VkCommandPool cmd_pool_{VK_NULL_HANDLE};
    VkCommandBuffer cmd_{VK_NULL_HANDLE};
    VkFence frame_fence_{VK_NULL_HANDLE};

    std::unordered_map<VkImage, VkImageView> color_views_;
    std::unordered_map<VkImage, VkFramebuffer> framebuffers_;

    std::vector<GltfGpuMesh> gltf_meshes_;
    std::unordered_map<std::string, int> gltf_path_to_slot_;
    // Inline procedural meshes (world-gen add_mesh) cached by entity id.
    std::unordered_map<std::uint32_t, int> inline_mesh_slots_;
};

}  // namespace ptv
