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
#include "ptv/render/FxRenderer.h"
#include "ptv/render/TextRenderer.h"
#include "ptv/render/TextureCache.h"
#include "ptv/scene/Scene.h"

namespace ptv {

class VulkanContext;
class World;
class XrRuntime;

/**
 * Forward renderer for the VR stage.
 *
 * The renderer owns:
 *   * a color + depth render pass (multiview when the swapchain is an array),
 *   * the scene pipelines (static / skinned x mirrored x alpha-test x
 *     double-sided), built lazily and cached,
 *   * per-frame resources: the frame uniform block (both eyes' matrices +
 *     environment), the instance buffer every draw indexes, the joint
 *     palettes,
 *   * the mesh cache (static + skinned, with per-material submeshes) and the
 *     material TextureCache,
 *   * framebuffers keyed by swapchain VkImage.
 *
 * It does not own the swapchains themselves; those are owned by
 * :class:`XrRuntime`. The main loop calls :meth:`draw_frame` once per XR
 * frame, passing in the two eye views and the acquired swapchain images.
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
        //   3 = unlit marker
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
        // True for a mirror reflection: same mesh and same joint palette,
        // model matrix premultiplied by the plane reflection. Selects the
        // front-face-culling pipeline so the reflection is not inside-out.
        bool mirrored{false};
    };

    struct EyeTarget {
        VkImage image{VK_NULL_HANDLE};
        /** Swapchain array layer of this eye (multiview: 0 and 1 of one image). */
        uint32_t array_layer{0};
        int32_t offset_x{0};
        int32_t offset_y{0};
        uint32_t width{0};
        uint32_t height{0};
        Mat4 view{1.0f};
        Mat4 projection{1.0f};
        /** Fragment density map image for this swapchain image (FFR), or null. */
        VkImage density_map{VK_NULL_HANDLE};
        uint32_t density_width{0};
        uint32_t density_height{0};
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

    /** Swapchain shape the renderer is created for. */
    struct TargetInfo {
        VkFormat color_format{VK_FORMAT_UNDEFINED};
        uint32_t width{0};
        uint32_t height{0};
        /** One array swapchain with both eyes as layers, drawn in a single
         *  multiview pass (VK_KHR_multiview); false = one pass per eye. */
        bool multiview{false};
        /** The swapchain images carry fragment density maps (FFR). */
        bool fragment_density_map{false};
    };

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize(VulkanContext& vk,
                    VkFormat color_format,
                    uint32_t eye_width,
                    uint32_t eye_height);
    bool initialize(VulkanContext& vk, const TargetInfo& target);
    void shutdown();

    /** Render the two eyes. The caller has already acquired+waited each
     *  swapchain image.
     *
     *  ``env`` carries the scene's sky + lighting; it is uploaded to the
     *  frame uniform block once per frame and drives both the gradient sky
     *  pass and the forward-lit geometry pass. */
    void draw_frame(const std::array<EyeTarget, 2>& eyes,
                    const std::vector<DrawItem>& items,
                    const RenderEnv& env);

    /** Where the player is looking this frame, used for culling + LOD.
     *
     *  ``view_proj`` is a single frustum that covers BOTH eyes (built from the
     *  HMD centre view with a widened FOV), so visibility is decided once per
     *  frame rather than per eye. When ``eye_view_proj`` is valid, culling
     *  uses the exact union of the two eye frustums instead. */
    struct ViewInfo {
        Vec3 camera_pos{0.0f, 0.0f, 0.0f};
        Mat4 view_proj{1.0f};
        bool valid{false};
        std::array<Mat4, 2> eye_view_proj{Mat4(1.0f), Mat4(1.0f)};
        bool eyes_valid{false};
        /** Vertical field of view of one eye (radians) and its height in
         *  pixels, for projected-size LOD. */
        float fov_y{1.6f};
        float viewport_height_px{1760.0f};
    };

    /** Per-frame rendering statistics, for telemetry + the adaptive governor. */
    struct FrameStats {
        std::uint32_t entities_considered{0};
        std::uint32_t drawn{0};
        std::uint32_t culled_frustum{0};
        std::uint32_t culled_distance{0};
        std::uint32_t culled_small{0};
        std::uint32_t visible_triangles{0};
        std::uint32_t resident_meshes{0};
        std::uint32_t resident_triangles{0};
        std::uint32_t streaming_pending{0};
        /** Entities drawn a second time as a mirror reflection. Reported so
         *  a mirror cannot silently eat the frame budget: this is the classic
         *  mobile-VR trap, and the number has to be measured, not assumed. */
        std::uint32_t mirror_reflections{0};
        /** Recorded for the last submitted frame, all views together. */
        std::uint32_t draw_calls{0};
        std::uint32_t pipeline_binds{0};
        std::uint32_t descriptor_binds{0};
        std::uint32_t instances{0};
        /** Submeshes drawn with the missing-texture material this frame. */
        std::uint32_t missing_texture_draws{0};
        /** Entities hidden because they stand in a closed room the player is
         *  not in or next to. */
        std::uint32_t culled_room{0};
        /** Merged static batches drawn (each replaces many entities). */
        std::uint32_t batches_drawn{0};
    };

    /** Merge the scene's static geometry (entities the compiler marked
     *  ``static_batch``: primitives and inline meshes) into one mesh per grid
     *  cell, room and material, and assign every static entity its closed
     *  room. Call after a scene is applied to the world. A member that later
     *  moves dissolves its batch (logged) and draws on its own again. */
    void build_static_batches(World& world, const std::vector<NavRegionSpec>& regions,
                              const std::vector<NavLinkSpec>& links);
    std::size_t static_batch_count() const { return batches_.size(); }

    /** MemoryGuard's level (0 green .. 3 critical). Yellow stops speculative
     *  prefetch and evicts long-unused streamable meshes; red evicts every
     *  invisible one above the always-resident floor and defers new
     *  high-detail loads; critical also draws visible meshes at their
     *  coarsest resident LOD. */
    void set_memory_pressure(int level) { memory_pressure_ = level; }
    int memory_pressure() const { return memory_pressure_; }
    /** Estimated bytes of meshes queued to load (MemoryGuard reservations). */
    std::uint64_t pending_stream_bytes();
    /** Meshes evicted since start (telemetry). */
    int evictions() const { return evictions_; }
    /** Queue every placed mesh's lowest LOD as an always-resident floor. */
    void request_floor_lods(World& world);

    /** GPU time of the most recently completed frame, from timestamp
     *  queries (0 when the queue cannot time), and how long the CPU sat in
     *  fence waits during that ``draw_frame`` -- subtracting it from the
     *  frame's wall time gives the CPU's own share. */
    float last_gpu_ms() const { return last_gpu_ms_; }
    float last_fence_wait_ms() const { return last_fence_wait_ms_; }
    bool gpu_timing_supported() const { return timestamp_pool_ != VK_NULL_HANDLE; }

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

    /** Draw materials whose texture failed to load with a magenta checker
     *  (verification and debug builds) instead of their factor colour. */
    void set_texture_diagnostics(bool magenta) { texture_diagnostics_ = magenta; }
    const TextureCache& textures() const { return textures_; }

    /** World-space strings for the next frame (consumed by draw_frame). */
    void submit_text(TextItem item) { text_items_.push_back(std::move(item)); }
    /** Drop this frame's text and effect quads (the texture gallery shows
     *  one asset on the bare sky, so a HUD panel cannot pass for it). */
    void drop_overlays() {
        text_items_.clear();
        fx_items_.clear();
    }
    /** Effect quads for the next frame (consumed by draw_frame), at most
     *  ``set_fx_budget`` of them. */
    void submit_fx(const FxQuad& q) { fx_items_.push_back(q); }
    void set_fx_budget(std::size_t max_quads) { fx_budget_ = max_quads; }
    std::uint32_t fx_quads_drawn() const { return fx_.quad_count(); }
    /** True when the last draw list included ``entity``'s mesh (telemetry). */
    bool drew_entity(std::uint32_t entity) const {
        return frame_entity_slot_.count(entity) != 0;
    }
    /** The glyph atlas, for measuring text; null when text is unavailable. */
    const TextAtlas* text_atlas() const {
        return text_.ready() ? &text_.atlas() : nullptr;
    }

    /** Comfort vignette for the next frame(s).
     *
     *  ``opacity`` 0 disables it. ``clear_half_angle_deg`` is the radius of
     *  the unobstructed view around each eye's forward axis and
     *  ``feather_deg`` the width of the soft edge beyond it -- angles, not
     *  screen fractions, so every headset occludes the same visual field. */
    void set_vignette(float opacity, float clear_half_angle_deg,
                      float feather_deg) {
        vignette_opacity_ = opacity;
        vignette_clear_deg_ = clear_half_angle_deg;
        vignette_feather_deg_ = feather_deg;
    }

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
     *  fall back to.
     *
     *  ``frame`` (optional) is the asset's measured model frame: the mesh is
     *  then scaled by height and pivoted onto the engine's frame, and cached
     *  under ``frame->unit_scale`` -- the same key the draw loop builds from
     *  ``CMesh::unit_scale``. */
    int load_glb_skinned(const std::string& resolved_path,
                         float target_scale_meters,
                         gltf_mesh_load::SkeletonData& out_skeleton,
                         const gltf_mesh_load::ModelFrame* frame = nullptr);

    /** True when ``slot`` holds skinned geometry. */
    bool mesh_slot_is_skinned(int slot) const;

    /** What one resident mesh slot holds (the texture verification gallery
     *  and telemetry). */
    struct SlotInfo {
        bool valid{false};
        bool skinned{false};
        float radius{0.0f};
        Vec3 center{0.0f};
        std::uint32_t triangles{0};
        int submeshes{0};
        int textured{0};
        int missing{0};
    };
    SlotInfo mesh_slot_info(int slot) const;

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
    bool multiview() const { return multiview_; }

    /** GPU memory the renderer itself holds (bytes): meshes, textures,
     *  render targets and per-frame buffers. */
    struct MemoryStats {
        std::uint64_t mesh_bytes{0};
        std::uint64_t texture_bytes{0};
        std::uint64_t target_bytes{0};
        std::uint64_t frame_bytes{0};
    };
    MemoryStats memory_stats() const;

private:
    /** Pipeline variant bits. */
    enum PipelineBits : std::uint32_t {
        kPipeSkinned = 1u,
        kPipeMirrored = 2u,
        kPipeAlphaTest = 4u,
        kPipeCullNone = 8u,
        kPipeVariants = 16u,
    };

    VkSampleCountFlagBits pick_sample_count();
    bool create_render_pass();
    bool create_descriptors();
    bool create_pipeline_layout();
    VkPipeline scene_pipeline(std::uint32_t bits);
    bool create_sky_pipeline();
    bool create_vignette_pipeline();
    /** Append reflected copies of the avatar (and anything tagged) for the
     *  nearest active mirror. Called at the end of build_items_from_world. */
    void append_mirror_reflections(World& world, std::vector<DrawItem>& out,
                                   const ViewInfo& view);
    bool ensure_joint_buffer(std::uint32_t matrix_capacity);
    bool ensure_instance_buffer(std::uint32_t instance_capacity);
    bool create_primitive_meshes();
    bool create_command_resources();
    bool create_depth_image();
    bool create_msaa_color_image();

    VkImageView get_or_create_color_view(VkImage img);
    VkFramebuffer get_or_create_framebuffer(const EyeTarget& eye);

    /** One material range of a mesh slot. */
    struct GpuSubmesh {
        uint32_t first_index{0};
        uint32_t index_count{0};
        int texture{-1};             // TextureCache id, -1 = untextured
        bool missing_texture{false}; // wanted a texture that failed to load
        float alpha_cutoff{0.0f};    // > 0 = alpha test (MASK, or BLEND as MASK)
        bool cull_none{false};
    };

    struct GltfGpuMesh {
        VkBuffer vb{VK_NULL_HANDLE};
        VmaAllocation vb_alloc{nullptr};
        VkBuffer ib{VK_NULL_HANDLE};
        VmaAllocation ib_alloc{nullptr};
        uint32_t index_count{0};
        // Bookkeeping for culling, telemetry and LRU eviction.
        uint32_t triangle_count{0};
        float local_radius{0.0f};   // bounding-sphere radius in mesh space
        Vec3 local_center{0.0f};    // its centre (AABB centre)
        uint64_t last_used_frame{0};
        bool evictable{false};      // false for built-in primitives / icons
        /** An asset's lowest LOD: always resident, never evicted, so a late
         *  load shows a coarser version and never nothing. */
        bool floor{false};
        // 32-byte packed skinned geometry; must be drawn with the skinned
        // pipeline, and drawing it with the static one would misread every
        // attribute past the position.
        bool skinned{false};
        std::string path;           // cache key, empty for procedural meshes
        /** UV dequantisation: uv = xform.xy + unorm16 * xform.zw. */
        Vec4 uv_xform{0.0f, 0.0f, 1.0f, 1.0f};
        std::vector<GpuSubmesh> submeshes;
        uint64_t gpu_bytes{0};
    };

    // --- Asynchronous mesh streaming ---------------------------------------
    // A worker thread does the expensive part (asset read + glTF parse + CPU
    // vertex build + texture decode); the main thread only performs the
    // Vulkan upload, capped per frame. This is what keeps a 13-asset scene
    // from stalling the very first frame for ~5 s and tripping Android's ANR
    // watchdog.
    struct StreamRequest {
        std::string path;
        /** Cache key: the path AND the scale it is loaded at. */
        std::string key;
        float target_scale_meters{1.0f};
        float unit_scale{0.0f};
        float priority{0.0f};  // distance in metres; smaller = sooner
    };
    struct DecodedTexture {
        std::string key;
        TextureData data;
        bool ok{false};
        std::string error;
    };
    struct StreamResult {
        std::string key;
        std::string path;
        std::vector<gltf_mesh_load::LitVertex> verts;
        std::vector<std::uint32_t> indices;
        gltf_mesh_load::MeshMaterials materials;
        std::vector<DecodedTexture> textures;
        bool ok{false};
    };

    /** Cache key for a mesh loaded at a given scale.
     *
     *  Scale is part of the key because the vertices are baked at load
     *  time. Keyed by path alone, the same GLB placed at two scales -- a
     *  weapon the world architect placed at 1.0 and the scene placed at
     *  0.24 -- shared whichever copy streamed in first, so the gun's size
     *  changed from launch to launch. */
    static std::string mesh_key(const std::string& path, float target_scale,
                                float unit_scale);

    void start_stream_worker();
    void stop_stream_worker();
    void stream_worker_main();
    /** Slot for an already-resident mesh, or -1. Marks it used this frame. */
    int resident_slot(const std::string& key);
    /** Queue ``path`` for background loading (no-op if resident/in flight). */
    void request_stream(const std::string& path, float target_scale_meters,
                        float unit_scale, float priority, bool floor = false);
    /** Free least-recently-used meshes until back under the resident budget. */
    void evict_to_budget();
    void destroy_mesh_slot(GltfGpuMesh& gm);

    /** The TextureRequest for one material's base-colour image. */
    TextureRequest texture_request(const std::string& glb_path,
                                   const gltf_mesh_load::MeshMaterials& mats,
                                   int image_index, const std::string& label) const;
    /** Decode (worker thread) every image ``mats`` needs that is not yet
     *  resident. */
    void decode_textures(const std::string& glb_path,
                         const gltf_mesh_load::MeshMaterials& mats,
                         std::vector<DecodedTexture>& out) const;
    /** Resolve each submesh's material to a GPU texture (upload decoded
     *  data, reuse a resident one, or decode synchronously). */
    void bind_materials(GltfGpuMesh& gm, const std::string& glb_path,
                        const gltf_mesh_load::MeshMaterials* mats,
                        std::vector<DecodedTexture>* decoded);

    /** Upload a finished lit-vertex mesh into the shared mesh cache and return
     *  its slot (or -1). Used by the GLB path, the inline-mesh path, and the
     *  built-in unit primitive meshes so buffer creation lives in one place.
     *  ``mats`` (optional) splits it into per-material submeshes. */
    int create_mesh_slot(const std::vector<gltf_mesh_load::LitVertex>& verts,
                         const std::vector<std::uint32_t>& indices,
                         const gltf_mesh_load::MeshMaterials* mats = nullptr,
                         const std::string& glb_path = std::string(),
                         std::vector<DecodedTexture>* decoded = nullptr);
    /** Same, for skinned vertices. Marks the slot ``skinned``. */
    int create_mesh_slot_skinned(
        const std::vector<gltf_mesh_load::SkinnedVertex>& verts,
        const std::vector<std::uint32_t>& indices,
        const gltf_mesh_load::MeshMaterials* mats = nullptr,
        const std::string& glb_path = std::string());
    int store_mesh_slot(GltfGpuMesh&& gm);
    bool upload_mesh_buffers(const void* vdata, std::size_t vbytes,
                             const std::uint32_t* indices, std::size_t index_count,
                             GltfGpuMesh& gm);
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
    bool multiview_{false};
    bool fdm_{false};
    // Anti-aliasing sample count, clamped to what the device supports.
    VkSampleCountFlagBits samples_{VK_SAMPLE_COUNT_1_BIT};

    VkRenderPass render_pass_{VK_NULL_HANDLE};
    // Set 0 = frame (UBO, joint palettes, instances), set 1 = material.
    // Shared by every scene pipeline and by the sky / vignette passes.
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    std::array<VkPipeline, kPipeVariants> pipelines_{};
    VkShaderModule vs_{VK_NULL_HANDLE};
    VkShaderModule fs_{VK_NULL_HANDLE};
    VkShaderModule skinned_vs_{VK_NULL_HANDLE};

    // Fullscreen gradient-sky pass (drawn before geometry each eye).
    VkPipeline sky_pipeline_{VK_NULL_HANDLE};
    VkShaderModule sky_vs_{VK_NULL_HANDLE};
    VkShaderModule sky_fs_{VK_NULL_HANDLE};

    // Comfort vignette: fullscreen pass drawn after geometry each eye,
    // reusing sky_vs_ for the fullscreen triangle.
    VkPipeline vignette_pipeline_{VK_NULL_HANDLE};
    VkShaderModule vignette_fs_{VK_NULL_HANDLE};
    float vignette_opacity_{0.0f};
    float vignette_clear_deg_{60.0f};
    float vignette_feather_deg_{20.0f};

    // World-space SDF text (menus, HUD numerals), drawn after geometry.
    TextRenderer text_{};
    std::vector<TextItem> text_items_;
    FxRenderer fx_{};
    std::vector<FxQuad> fx_items_;
    std::size_t fx_budget_{1024};

    TextureCache textures_{};
    bool texture_diagnostics_{false};

    // Frame resources (set 0). The frame UBO and the instance buffer are
    // persistently mapped and rewritten each frame after the fence wait.
    VkDescriptorSetLayout frame_set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool desc_pool_{VK_NULL_HANDLE};
    VkDescriptorSet frame_set_{VK_NULL_HANDLE};
    VkBuffer frame_ubo_{VK_NULL_HANDLE};
    VmaAllocation frame_ubo_alloc_{nullptr};
    void* frame_ubo_mapped_{nullptr};

    VkBuffer joint_ssbo_{VK_NULL_HANDLE};
    VmaAllocation joint_ssbo_alloc_{nullptr};
    void* joint_ssbo_mapped_{nullptr};
    std::uint32_t joint_ssbo_capacity_{0};  // in matrices
    std::vector<Mat4> joint_palettes_;

    VkBuffer instance_ssbo_{VK_NULL_HANDLE};
    VmaAllocation instance_ssbo_alloc_{nullptr};
    void* instance_ssbo_mapped_{nullptr};
    std::uint32_t instance_capacity_{0};

    // Built-in unit meshes. Values are slots into ``gltf_meshes_``.
    int prim_slot_cube_{-1};
    int prim_slot_sphere_{-1};
    int prim_slot_cylinder_{-1};
    int prim_slot_plane_{-1};
    int prim_slot_ramp_{-1};

    // Depth (multisampled when MSAA is on), one layer per view.
    VkImage depth_image_{VK_NULL_HANDLE};
    VmaAllocation depth_alloc_{nullptr};
    VkImageView depth_view_{VK_NULL_HANDLE};

    // Multisampled color target (only allocated when samples_ > 1); resolved
    // into the swapchain image at the end of the render pass.
    VkImage msaa_color_image_{VK_NULL_HANDLE};
    VmaAllocation msaa_color_alloc_{nullptr};
    VkImageView msaa_color_view_{VK_NULL_HANDLE};
    std::uint64_t target_bytes_{0};

    VkCommandPool cmd_pool_{VK_NULL_HANDLE};
    VkCommandBuffer cmd_{VK_NULL_HANDLE};
    VkFence frame_fence_{VK_NULL_HANDLE};

    // --- Frames in flight ----------------------------------------------------
    // Everything the CPU rewrites each frame exists once per frame in flight.
    // The members above (cmd_, frame_fence_, frame_set_, frame_ubo_*,
    // joint_ssbo_*, instance_ssbo_*) always name the ACTIVE slot;
    // select_frame_slot swaps them, so recording code never indexes.
    static constexpr int kMaxFramesInFlight = 2;
    struct FrameSlot {
        VkCommandBuffer cmd{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        VkDescriptorSet set{VK_NULL_HANDLE};
        VkBuffer ubo{VK_NULL_HANDLE};
        VmaAllocation ubo_alloc{nullptr};
        void* ubo_mapped{nullptr};
        VkBuffer joints{VK_NULL_HANDLE};
        VmaAllocation joints_alloc{nullptr};
        void* joints_mapped{nullptr};
        std::uint32_t joints_capacity{0};
        VkBuffer instances{VK_NULL_HANDLE};
        VmaAllocation instances_alloc{nullptr};
        void* instances_mapped{nullptr};
        std::uint32_t instances_capacity{0};
        bool timestamps_written{false};
    };
    std::array<FrameSlot, kMaxFramesInFlight> frame_slots_{};
    int frame_slot_{0};
    int frames_in_flight_{1};
    void select_frame_slot(int slot);
    // Evicted meshes wait here until no frame in flight can still read them.
    struct RetiredMesh {
        GltfGpuMesh mesh;
        std::uint64_t frame{0};
    };
    std::vector<RetiredMesh> retired_meshes_;

    // --- Static batching + room culling --------------------------------------
    struct StaticBatch {
        int slot{-1};
        Vec3 center{0.0f};
        float radius{0.0f};
        int room{-1};
        float material_id{0.0f};
        bool valid{true};
        std::uint32_t triangles{0};
        std::vector<std::pair<std::uint32_t, Transform>> members;
    };
    std::vector<StaticBatch> batches_;
    struct RegionBox {
        Vec3 min{0.0f};
        Vec3 max{0.0f};
        float floor_y{0.0f};
        bool closed{false};
    };
    std::vector<RegionBox> regions_;
    std::vector<std::vector<int>> region_links_;
    std::vector<char> room_visible_;
    int memory_pressure_{0};
    int evictions_{0};
    std::unordered_set<std::string> floor_keys_;
    Vec3 prev_cam_{0.0f};
    Vec3 cam_velocity_{0.0f};
    bool have_prev_cam_{false};
    bool dissolve_warned_{false};
    int region_of(const Vec3& p) const;
    void check_static_batches(World& world);
    /** Out-of-memory recovery: free retired and invisible streamable meshes
     *  (never the floor) and purge the heap. True when anything was freed,
     *  so the caller retries its allocation once. */
    bool recover_from_allocation_failure();

public:
    /** Frames the CPU may record ahead of the GPU (1 or 2). Desktop builds
     *  stay at 1: Quest Link has shown white frames when a swapchain image is
     *  released with its rendering still queued. */
    void set_frames_in_flight(int n);
    int frames_in_flight() const { return frames_in_flight_; }

private:

    // Two timestamps per frame (start, end of the command buffer).
    VkQueryPool timestamp_pool_{VK_NULL_HANDLE};
    float timestamp_period_ns_{0.0f};
    std::uint64_t timestamp_mask_{~0ull};
    float last_gpu_ms_{0.0f};
    float last_fence_wait_ms_{0.0f};

    std::unordered_map<std::uint64_t, VkImageView> color_views_;
    std::unordered_map<std::uint64_t, VkImageView> density_views_;
    std::unordered_map<std::uint64_t, VkFramebuffer> framebuffers_;

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
    // Which mesh slot each entity resolved to THIS frame. Lets the mirror
    // pass reuse the LOD the main pass already picked, and skip anything not
    // yet streamed in, without re-deriving any paths.
    std::unordered_map<std::uint32_t, int> frame_entity_slot_;

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
