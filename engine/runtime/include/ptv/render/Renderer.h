#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/math/Math.h"
#include "ptv/render/GltfMeshLoad.h"

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

    /** A single point light forwarded to the fragment shader (max 8). */
    struct PointLight {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float range{20.0f};
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity{1.0f};
    };

    /** Per-scene environment (sky + lighting + fog) that the renderer feeds to
     *  the shaders through a uniform buffer. Built by Application from the
     *  loaded ``SceneDoc`` so what the world-generation agent authored (sky
     *  gradient, sun, ambient, point lights) actually drives the look. */
    struct RenderEnv {
        Vec3 sky_top{0.05f, 0.07f, 0.15f};
        Vec3 sky_horizon{0.45f, 0.55f, 0.75f};
        // Direction the sunlight travels (points away from the sun, i.e.
        // downward-ish). The shaders negate it to get the direction toward
        // the sun.
        Vec3 sun_direction{-0.35f, -0.80f, -0.40f};
        Vec3 sun_color{1.0f, 0.97f, 0.9f};
        float sun_intensity{1.0f};
        // Ground/bounce color used as the lower hemisphere of the ambient term.
        Vec3 ambient_ground{0.20f, 0.18f, 0.15f};
        float ambient_intensity{0.25f};
        // Distance fog reuses ``sky_horizon`` as its color (so geometry melts
        // into the sky); this is just how thick it is.
        float fog_density{0.012f};
        // Global brightness multiplier applied before tone mapping (author-
        // controlled; 1.0 = neutral).
        float exposure{1.0f};
        bool has_sky{false};
        std::vector<PointLight> points;
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
     *  wait for the GPU on the main thread (simple, low-throughput).
     *
     *  ``env`` carries the scene's sky + lighting; it is uploaded to a uniform
     *  buffer once per frame and drives both the gradient sky pass and the
     *  forward-lit geometry pass. */
    void draw_frame(const std::array<EyeTarget, 2>& eyes,
                    const std::vector<DrawItem>& items,
                    const RenderEnv& env);

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
    VkSampleCountFlagBits pick_sample_count();
    bool create_render_pass();
    bool create_descriptors();
    bool create_pipeline();
    bool create_sky_pipeline();
    bool create_cube_mesh();
    bool create_primitive_meshes();
    bool create_command_resources();
    bool create_depth_image();
    bool create_msaa_color_image();

    VkImageView get_or_create_color_view(VkImage img);
    VkFramebuffer get_or_create_framebuffer(VkImage img);

    struct GltfGpuMesh {
        VkBuffer vb{VK_NULL_HANDLE};
        VmaAllocation vb_alloc{nullptr};
        VkBuffer ib{VK_NULL_HANDLE};
        VmaAllocation ib_alloc{nullptr};
        uint32_t index_count{0};
    };
    /** Upload a finished lit-vertex mesh into the shared mesh cache and return
     *  its slot (or -1). Used by the GLB path, the inline-mesh path, and the
     *  built-in unit primitive meshes so buffer creation lives in one place. */
    int create_mesh_slot(const std::vector<gltf_mesh_load::LitVertex>& verts,
                         const std::vector<std::uint32_t>& indices);
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
    // Anti-aliasing sample count, clamped to what the device supports.
    VkSampleCountFlagBits samples_{VK_SAMPLE_COUNT_1_BIT};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};

    // Fullscreen gradient-sky pass (drawn before geometry each eye).
    VkPipelineLayout sky_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline sky_pipeline_{VK_NULL_HANDLE};
    VkShaderModule sky_vs_{VK_NULL_HANDLE};
    VkShaderModule sky_fs_{VK_NULL_HANDLE};

    VkShaderModule vs_{VK_NULL_HANDLE};
    VkShaderModule fs_{VK_NULL_HANDLE};

    // Environment/lighting uniform buffer (set 0, binding 0), persistently
    // mapped and refreshed once per frame from the RenderEnv.
    VkDescriptorSetLayout env_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool desc_pool_{VK_NULL_HANDLE};
    VkDescriptorSet env_set_{VK_NULL_HANDLE};
    VkBuffer env_ubo_{VK_NULL_HANDLE};
    VmaAllocation env_ubo_alloc_{nullptr};
    void* env_ubo_mapped_{nullptr};

    VkBuffer cube_vb_{VK_NULL_HANDLE};
    VmaAllocation cube_vb_alloc_{nullptr};
    VkBuffer cube_ib_{VK_NULL_HANDLE};
    VmaAllocation cube_ib_alloc_{nullptr};
    uint32_t cube_index_count_{0};

    // Built-in unit meshes for world-gen primitive kinds (box uses the cube
    // path via mesh_slot = -1). Values are slots into ``gltf_meshes_``.
    int prim_slot_sphere_{-1};
    int prim_slot_cylinder_{-1};
    int prim_slot_plane_{-1};
    int prim_slot_ramp_{-1};

    // Single-sampled depth used when MSAA is unavailable; otherwise multisampled.
    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_alloc_{nullptr};
    VkImageView depth_view_{VK_NULL_HANDLE};

    // Multisampled color target (only allocated when samples_ > 1); resolved
    // into the swapchain image at the end of the render pass.
    VkImage msaa_color_image_{VK_NULL_HANDLE};
    VmaAllocation msaa_color_alloc_{nullptr};
    VkImageView msaa_color_view_{VK_NULL_HANDLE};

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
