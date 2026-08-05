#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ptv/math/Math.h"
#include "ptv/render/GltfMeshLoad.h"
#include "ptv/render/GltfSkinnedLoad.h"
#include "ptv/scene/Scene.h"

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
        // Skinning. -1 = static geometry, drawn with the plain pipeline.
        // >=0 = first joint matrix for this item inside the frame's shared
        // joint-palette buffer; see Renderer::push_joint_palette. The
        // referenced mesh slot must have been created as skinned.
        int joint_base{-1};
        int joint_count{0};
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

    /** Where the player is looking this frame, used for culling + LOD.
     *
     *  ``view_proj`` is a single frustum that covers BOTH eyes (built from the
     *  HMD centre view with a widened FOV), so visibility is decided once per
     *  frame rather than per eye. */
    struct ViewInfo {
        Vec3 camera_pos{0.0f, 0.0f, 0.0f};
        Mat4 view_proj{1.0f};
        bool valid{false};
    };

    /** Per-frame rendering statistics, for telemetry + the adaptive governor. */
    struct FrameStats {
        std::uint32_t entities_considered{0};
        std::uint32_t drawn{0};
        std::uint32_t culled_frustum{0};
        std::uint32_t culled_distance{0};
        std::uint32_t visible_triangles{0};
        std::uint32_t resident_meshes{0};
        std::uint32_t resident_triangles{0};
        std::uint32_t streaming_pending{0};
    };

    /** Install the scene's device budget (LOD distances, cull distance,
     *  resident budget, eviction policy). Safe to call before initialize. */
    void set_performance(const ScenePerformance& perf);

    /** Global LOD bias applied on top of the scene's switch distances. The
     *  adaptive governor raises this when frame time slips (higher = coarser
     *  meshes sooner). */
    void set_lod_bias(float bias) { lod_bias_ = bias; }
    float lod_bias() const { return lod_bias_; }

    /** Runtime override of the scene cull distance (governor-controlled). */
    void set_cull_distance(float meters) { cull_distance_m_ = meters; }
    float cull_distance() const { return cull_distance_m_; }

    const FrameStats& frame_stats() const { return stats_; }

    /** Build a list of DrawItems from the ECS.
     *
     *  Entities are frustum- and distance-culled, and each GLB entity picks
     *  the LOD appropriate for its on-screen size. Meshes that are not yet
     *  resident are requested from the streaming loader and skipped for this
     *  frame (or drawn at a coarser LOD that already is resident) -- the draw
     *  loop NEVER blocks on disk I/O. */
    void build_items_from_world(World& world, std::vector<DrawItem>& out,
                                const ViewInfo& view);

    /** Upload a bounded number of finished streaming loads and evict meshes
     *  when over the resident budget. Called once per frame; the time budget
     *  keeps the Android event pump alive (a stalled pump is what produces
     *  "app isn't responding"). */
    void pump_streaming();

    /** Pre-load (or fetch from cache) a GLB mesh for use as a UI icon /
     *  manually-placed DrawItem. Returns the cache slot to set on
     *  ``DrawItem::mesh_slot``, or -1 on failure (file not found, no
     *  triangles, etc.). The mesh is rescaled so its longest axis is
     *  ``target_scale_meters`` -- see GltfMeshLoad. Safe to call multiple
     *  times with the same path; the second call returns the cached slot. */
    int load_glb(const std::string& resolved_path, float target_scale_meters);

    /** Synchronously load a rigged GLB into the mesh cache.
     *
     *  Returns a slot for ``DrawItem::mesh_slot`` (or -1) and fills
     *  ``out_skeleton`` with the hierarchy the pose system needs. Unlike the
     *  static streaming path this blocks, because a skinned entity cannot be
     *  drawn at all until its skeleton exists -- there is no coarser LOD to
     *  fall back to. */
    int load_glb_skinned(const std::string& resolved_path,
                         float target_scale_meters,
                         gltf_mesh_load::SkeletonData& out_skeleton);

    /** True when ``slot`` holds skinned geometry (48-byte vertices). */
    bool mesh_slot_is_skinned(int slot) const;

    // --- Per-frame joint palettes -------------------------------------------
    //
    // Every skinned draw this frame shares one storage buffer; an item just
    // records where its matrices start. That keeps skinned meshes off the
    // per-draw descriptor churn that would otherwise make each one its own
    // binding change, which matters directly to the draw-call budget in
    // ScenePerformance.

    /** Drop last frame's palettes. Call once per frame before building items. */
    void begin_joint_palettes();
    /** Append ``count`` joint matrices; returns the base index to store in
     *  ``DrawItem::joint_base``, or -1 if the arena is full. */
    int push_joint_palette(const Mat4* matrices, int count);

    VkFormat color_format() const { return color_format_; }
    VkRenderPass render_pass() const { return render_pass_; }

private:
    VkSampleCountFlagBits pick_sample_count();
    bool create_render_pass();
    bool create_descriptors();
    bool create_pipeline();
    bool create_skinned_pipeline();
    bool create_sky_pipeline();
    bool ensure_joint_buffer(std::uint32_t matrix_capacity);
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
        // Bookkeeping for culling, telemetry and LRU eviction.
        uint32_t triangle_count{0};
        float local_radius{0.0f};   // bounding-sphere radius in mesh space
        uint64_t last_used_frame{0};
        bool evictable{false};      // false for built-in primitives / icons
        // 48-byte SkinnedVertex geometry; must be drawn with the skinned
        // pipeline, and drawing it with the static one would misread every
        // attribute past the position.
        bool skinned{false};
        std::string path;           // cache key, empty for procedural meshes
    };

    // --- Asynchronous mesh streaming ---------------------------------------
    // A worker thread does the expensive part (asset read + glTF parse + CPU
    // vertex build); the main thread only performs the Vulkan upload, capped
    // per frame. This is what keeps a 13-asset scene from stalling the very
    // first frame for ~5 s and tripping Android's ANR watchdog.
    struct StreamRequest {
        std::string path;
        float target_scale_meters{1.0f};
        float priority{0.0f};  // distance in metres; smaller = sooner
    };
    struct StreamResult {
        std::string path;
        std::vector<gltf_mesh_load::LitVertex> verts;
        std::vector<std::uint32_t> indices;
        bool ok{false};
    };

    void start_stream_worker();
    void stop_stream_worker();
    void stream_worker_main();
    /** Slot for an already-resident mesh, or -1. Marks it used this frame. */
    int resident_slot(const std::string& path);
    /** Queue ``path`` for background loading (no-op if resident/in flight). */
    void request_stream(const std::string& path, float target_scale_meters,
                        float priority);
    /** Free least-recently-used meshes until back under the resident budget. */
    void evict_to_budget();
    void destroy_mesh_slot(GltfGpuMesh& gm);
    /** Upload a finished lit-vertex mesh into the shared mesh cache and return
     *  its slot (or -1). Used by the GLB path, the inline-mesh path, and the
     *  built-in unit primitive meshes so buffer creation lives in one place. */
    int create_mesh_slot(const std::vector<gltf_mesh_load::LitVertex>& verts,
                         const std::vector<std::uint32_t>& indices);
    /** Same, for 48-byte skinned vertices. Marks the slot ``skinned``. */
    int create_mesh_slot_skinned(
        const std::vector<gltf_mesh_load::SkinnedVertex>& verts,
        const std::vector<std::uint32_t>& indices);
    /** Shared body of the two above. Type-erased over the vertex struct,
     *  which is safe because LitVertex and SkinnedVertex both begin with
     *  ``float pos[3]`` -- the only field this needs for bounds. */
    int create_mesh_slot_impl(const void* verts, std::size_t vert_count,
                              std::size_t vert_stride,
                              const std::uint32_t* indices,
                              std::size_t index_count, bool skinned);
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
    // Shared by the static and skinned geometry pipelines: same descriptor
    // set, same push-constant block, only the vertex input and vertex shader
    // differ.
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipeline skinned_pipeline_{VK_NULL_HANDLE};

    // Fullscreen gradient-sky pass (drawn before geometry each eye).
    VkPipelineLayout sky_pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline sky_pipeline_{VK_NULL_HANDLE};
    VkShaderModule sky_vs_{VK_NULL_HANDLE};
    VkShaderModule sky_fs_{VK_NULL_HANDLE};

    VkShaderModule vs_{VK_NULL_HANDLE};
    VkShaderModule fs_{VK_NULL_HANDLE};
    VkShaderModule skinned_vs_{VK_NULL_HANDLE};

    // Environment/lighting uniform buffer (set 0, binding 0), persistently
    // mapped and refreshed once per frame from the RenderEnv.
    VkDescriptorSetLayout env_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool desc_pool_{VK_NULL_HANDLE};
    VkDescriptorSet env_set_{VK_NULL_HANDLE};
    VkBuffer env_ubo_{VK_NULL_HANDLE};
    VmaAllocation env_ubo_alloc_{nullptr};
    void* env_ubo_mapped_{nullptr};

    // Joint matrix palettes for every skinned draw this frame (set 0,
    // binding 1). Host-visible and persistently mapped like the env UBO, and
    // refreshed in draw_frame after the fence wait so the previous frame has
    // finished reading it.
    VkBuffer joint_ssbo_{VK_NULL_HANDLE};
    VmaAllocation joint_ssbo_alloc_{nullptr};
    void* joint_ssbo_mapped_{nullptr};
    std::uint32_t joint_ssbo_capacity_{0};  // in matrices
    std::vector<Mat4> joint_palettes_;

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
    // Slots freed by eviction, reused before growing ``gltf_meshes_``.
    std::vector<int> free_mesh_slots_;

    // --- Device budget + adaptive quality ----------------------------------
    ScenePerformance perf_{};
    float lod_bias_{0.0f};
    float cull_distance_m_{100000.0f};
    std::uint64_t frame_index_{0};
    std::uint32_t resident_triangles_{0};
    FrameStats stats_{};
    // Per-entity LOD hysteresis: remembers the level chosen last frame so a
    // mesh sitting exactly on a switch distance does not flicker between LODs.
    std::unordered_map<std::uint32_t, int> entity_lod_;

    // --- Streaming worker ---------------------------------------------------
    std::thread stream_thread_;
    std::mutex stream_mutex_;
    std::condition_variable stream_cv_;
    std::vector<StreamRequest> stream_queue_;
    std::vector<StreamResult> stream_done_;
    std::unordered_set<std::string> stream_inflight_;
    std::unordered_set<std::string> stream_failed_;
    std::atomic<bool> stream_stop_{false};
};

}  // namespace ptv
