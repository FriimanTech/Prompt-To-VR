#include "ptv/render/Renderer.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "ptv/core/Logger.h"
#include "ptv/ecs/TransformOffset.h"
#include "ptv/ecs/World.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/GltfMeshLoad.h"
#include "ptv/render/VulkanContext.h"

#include "VisualTagTable.h"

namespace ptv {

namespace {

#ifndef PTV_SHADER_DIR
#define PTV_SHADER_DIR "shaders"
#endif

// Shaders are read through the process-global asset source so the same code
// works on desktop (filesystem) and on Quest (APK assets via AAssetManager).
// The default FilesystemAssetSource reproduces the historical ifstream read.
std::vector<uint8_t> read_file_bytes(const std::string& path) {
    auto bytes = asset_source().read_bytes(path);
    if (!bytes) {
        PTV_ERROR("Could not open shader SPIR-V: {}", path);
        return {};
    }
    return std::move(*bytes);
}

VkShaderModule make_shader(VkDevice dev, const std::string& path) {
    auto bytes = read_file_bytes(path);
    if (bytes.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = bytes.size();
    sci.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &sci, nullptr, &m) != VK_SUCCESS) {
        PTV_ERROR("vkCreateShaderModule failed for {}", path);
        return VK_NULL_HANDLE;
    }
    return m;
}

// Push constant layout must match scene.vert/frag.
struct PushConstants {
    Mat4 view_proj;
    Mat4 model;
    Vec4 color;
    // params.x = material_id (see Renderer::DrawItem). .yzw reserved.
    Vec4 params;
};

// Push constant for the fullscreen sky pass (matches sky.frag).
struct SkyPush {
    Mat4 inv_view_proj;
};

// Push constant for the comfort vignette (matches vignette.frag).
struct VignettePush {
    Mat4 inv_proj;
    Vec4 params;  // clear half-angle rad, feather rad, opacity, unused
    Vec4 color;
};

constexpr int kMaxPointLights = 8;

// Environment/lighting UBO. Layout MUST match the ``EnvUBO`` block in
// scene.frag / sky.frag (std140: every member is a vec4 so there is no
// padding ambiguity).
struct GpuPointLight {
    float pos_range[4];        // xyz = world position, w = range (m)
    float color_intensity[4];  // rgb = color, w = intensity
};
struct GpuEnv {
    float sun_dir[4];      // xyz = travel direction (normalized), w unused
    float sun_color[4];    // rgb = color, w = intensity
    float sky_top[4];      // rgb = zenith color, w = exposure
    float sky_horizon[4];  // rgb = horizon/fog color, w = fog density
    float ambient[4];      // rgb = ground bounce color, w = ambient intensity
    float camera_pos[4];   // xyz = camera world pos, w = active point count
    GpuPointLight points[kMaxPointLights];
};

// Shared vertex layout for both the procedural cube path and the GLB mesh
// path. Keeping a single layout means we only need one graphics pipeline
// and one vertex-input description; the cube simply ships pure-white
// colors so its appearance is driven entirely by the per-DrawItem tint
// (push constant), while GLBs ship per-vertex baseColor from their
// source material so each Tripo asset renders with its design palette.
struct Vertex {
    float pos[3];
    float normal[3];
    float color[4];
};
static_assert(sizeof(Vertex) == sizeof(gltf_mesh_load::LitVertex),
              "Renderer::Vertex and LitVertex must match (shared pipeline)");

// A unit cube centered at the origin with side length 1. Each face gets a
// flat outward normal (6 faces x 4 vertices = 24 unique vertices), which
// makes lighting look correct without smoothing. Color is white so the
// per-item DrawItem.color tint controls the look unmodified.
const std::array<Vertex, 24> kCubeVerts = {{
    // +X
    {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    // -X
    {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    // +Y (top)
    {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    // -Y
    {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f,1.0f,1.0f,1.0f}},
    // +Z
    {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    // -Z
    {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f,1.0f,1.0f,1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f,1.0f,1.0f,1.0f}},
}};

const std::array<uint16_t, 36> kCubeIndices = {{
    0,1,2, 0,2,3,       // +X
    4,5,6, 4,6,7,       // -X
    8,9,10, 8,10,11,    // +Y
    12,13,14, 12,14,15, // -Y
    16,17,18, 16,18,19, // +Z
    20,21,22, 20,22,23, // -Z
}};

void upload_buffer(VulkanContext& vk, VkBuffer buf, VmaAllocation alloc,
                   const void* data, size_t bytes) {
    void* mapped = nullptr;
    vmaMapMemory(vk.allocator(), alloc, &mapped);
    std::memcpy(mapped, data, bytes);
    vmaUnmapMemory(vk.allocator(), alloc);
    (void)buf;
}

// ---------------------------------------------------------------------------
// Built-in unit primitive meshes for the world-generation ``spawn_primitive``
// kinds. Each is authored at unit extent (fits the [-0.5, 0.5]^3 box, matching
// the cube) so ``build_items_from_world`` can scale by the primitive's ``size``
// exactly like the box path. Vertices ship white so the per-DrawItem tint
// (the primitive's color) drives the final look, identical to the cube.
// ---------------------------------------------------------------------------

using LitVertex = gltf_mesh_load::LitVertex;

void push_vertex(std::vector<LitVertex>& v, const Vec3& p, const Vec3& n) {
    LitVertex lv{};
    lv.pos[0] = p.x; lv.pos[1] = p.y; lv.pos[2] = p.z;
    lv.normal[0] = n.x; lv.normal[1] = n.y; lv.normal[2] = n.z;
    lv.color[0] = lv.color[1] = lv.color[2] = lv.color[3] = 1.0f;
    v.push_back(lv);
}

// Emit a flat quad (two triangles) with a single face normal.
void push_quad(std::vector<LitVertex>& v, std::vector<uint32_t>& idx,
               const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
               const Vec3& n) {
    const uint32_t base = static_cast<uint32_t>(v.size());
    push_vertex(v, a, n);
    push_vertex(v, b, n);
    push_vertex(v, c, n);
    push_vertex(v, d, n);
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

// UV sphere of radius 0.5 (diameter 1) with smooth (positional) normals.
void make_sphere(std::vector<LitVertex>& v, std::vector<uint32_t>& idx,
                 int stacks = 12, int slices = 20) {
    const float r = 0.5f;
    for (int i = 0; i <= stacks; ++i) {
        const float phi = 3.14159265358979f * static_cast<float>(i) / stacks;
        const float y = std::cos(phi);
        const float rs = std::sin(phi);
        for (int j = 0; j <= slices; ++j) {
            const float theta =
                2.0f * 3.14159265358979f * static_cast<float>(j) / slices;
            Vec3 n{rs * std::cos(theta), y, rs * std::sin(theta)};
            push_vertex(v, n * r, n);
        }
    }
    const int cols = slices + 1;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = static_cast<uint32_t>(i * cols + j);
            const uint32_t b = static_cast<uint32_t>((i + 1) * cols + j);
            // CCW-outward winding (matches the cube / glTF convention so back-
            // face culling keeps the outside visible, not the inside).
            idx.insert(idx.end(), {a, a + 1, b + 1, a, b + 1, b});
        }
    }
}

// Capped cylinder: diameter 1 in X/Z, height 1 in Y (y in [-0.5, 0.5]).
void make_cylinder(std::vector<LitVertex>& v, std::vector<uint32_t>& idx,
                   int seg = 24) {
    const float r = 0.5f, hy = 0.5f;
    // Side wall (radial normals), duplicated ring at top/bottom.
    const uint32_t side_base = static_cast<uint32_t>(v.size());
    for (int j = 0; j <= seg; ++j) {
        const float t = 2.0f * 3.14159265358979f * static_cast<float>(j) / seg;
        const float cx = std::cos(t), cz = std::sin(t);
        Vec3 n{cx, 0.0f, cz};
        push_vertex(v, Vec3{cx * r, -hy, cz * r}, n);
        push_vertex(v, Vec3{cx * r, hy, cz * r}, n);
    }
    for (int j = 0; j < seg; ++j) {
        const uint32_t a = side_base + static_cast<uint32_t>(j * 2);
        idx.insert(idx.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    // Caps (fans).
    auto cap = [&](float y, const Vec3& n, bool flip) {
        const uint32_t center = static_cast<uint32_t>(v.size());
        push_vertex(v, Vec3{0.0f, y, 0.0f}, n);
        for (int j = 0; j <= seg; ++j) {
            const float t =
                2.0f * 3.14159265358979f * static_cast<float>(j) / seg;
            push_vertex(v, Vec3{std::cos(t) * r, y, std::sin(t) * r}, n);
        }
        for (int j = 0; j < seg; ++j) {
            const uint32_t a = center + 1 + static_cast<uint32_t>(j);
            if (flip)
                idx.insert(idx.end(), {center, a + 1, a});
            else
                idx.insert(idx.end(), {center, a, a + 1});
        }
    };
    // Top cap needs the flipped winding and bottom the natural winding to keep
    // both facing outward (CCW-outward, matching the cube convention).
    cap(hy, Vec3{0.0f, 1.0f, 0.0f}, true);
    cap(-hy, Vec3{0.0f, -1.0f, 0.0f}, false);
}

// Two-sided unit plane in the XZ plane (extent 1x1, y = 0).
void make_plane(std::vector<LitVertex>& v, std::vector<uint32_t>& idx) {
    const float h = 0.5f;
    push_quad(v, idx, {-h, 0.0f, -h}, {-h, 0.0f, h}, {h, 0.0f, h},
              {h, 0.0f, -h}, {0.0f, 1.0f, 0.0f});
    push_quad(v, idx, {-h, 0.0f, -h}, {h, 0.0f, -h}, {h, 0.0f, h},
              {-h, 0.0f, h}, {0.0f, -1.0f, 0.0f});
}

// Right-triangular-prism ramp inside the unit box: low at -Z, high at +Z.
void make_ramp(std::vector<LitVertex>& v, std::vector<uint32_t>& idx) {
    const float h = 0.5f;
    const Vec3 L0{-h, -h, -h}, L1{-h, -h, h}, L2{-h, h, h};
    const Vec3 R0{h, -h, -h}, R1{h, -h, h}, R2{h, h, h};
    push_quad(v, idx, L0, R0, R1, L1, Vec3{0.0f, -1.0f, 0.0f});   // bottom
    push_quad(v, idx, L1, R1, R2, L2, Vec3{0.0f, 0.0f, 1.0f});    // back wall
    // Slope normal MUST match push_quad's winding (a=L2,b=R2,c=R0) so the
    // walkable top face shades up-and-back, not inverted.
    const Vec3 slope = glm::normalize(glm::cross(R2 - L2, R0 - L2));
    push_quad(v, idx, L2, R2, R0, L0, slope);                     // slope
    {  // -X side triangle
        const uint32_t b = static_cast<uint32_t>(v.size());
        push_vertex(v, L0, Vec3{-1, 0, 0});
        push_vertex(v, L1, Vec3{-1, 0, 0});
        push_vertex(v, L2, Vec3{-1, 0, 0});
        idx.insert(idx.end(), {b, b + 1, b + 2});
    }
    {  // +X side triangle
        const uint32_t b = static_cast<uint32_t>(v.size());
        push_vertex(v, R0, Vec3{1, 0, 0});
        push_vertex(v, R2, Vec3{1, 0, 0});
        push_vertex(v, R1, Vec3{1, 0, 0});
        idx.insert(idx.end(), {b, b + 1, b + 2});
    }
}

}  // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialize(VulkanContext& vk,
                          VkFormat color_format,
                          uint32_t eye_width,
                          uint32_t eye_height) {
    vk_ = &vk;
    color_format_ = color_format;
    eye_width_ = eye_width;
    eye_height_ = eye_height;
    samples_ = pick_sample_count();
    if (!create_render_pass()) return false;
    if (!create_descriptors()) return false;
    if (!create_pipeline()) return false;
    if (!create_skinned_pipeline()) return false;
    if (!create_sky_pipeline()) return false;
    // Optional: a missing vignette shader costs the comfort option, not the
    // game.
    if (!create_vignette_pipeline()) {
        PTV_WARN("Comfort vignette pipeline unavailable; vignettes disabled");
    }
    if (!create_cube_mesh()) return false;
    if (!create_primitive_meshes()) return false;
    if (!create_depth_image()) return false;
    if (!create_msaa_color_image()) return false;
    if (!create_command_resources()) return false;
    start_stream_worker();
    PTV_INFO("Renderer ready: {}x{} per eye, color fmt {}, MSAA {}x",
             eye_width_, eye_height_, static_cast<int>(color_format_),
             static_cast<int>(samples_));
    return true;
}

VkSampleCountFlagBits Renderer::pick_sample_count() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vk_->physical_device(), &props);
    // Only levels supported for BOTH color and depth framebuffers are usable
    // for our resolve pass.
    const VkSampleCountFlags counts =
        props.limits.framebufferColorSampleCounts &
        props.limits.framebufferDepthSampleCounts;

    // The scene's performance tier decides how much MSAA we can afford: 4x is
    // a heavy bandwidth cost on a mobile tiler, so large/massive scenes ask
    // for 2x (or 1x) instead of always taking the maximum the GPU advertises.
    // Clamped to what the device actually supports, and never above 4x.
    int requested = perf_.msaa_samples > 0 ? perf_.msaa_samples : 4;
    requested = std::min(requested, 4);
    if (requested >= 4 && (counts & VK_SAMPLE_COUNT_4_BIT)) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if (requested >= 2 && (counts & VK_SAMPLE_COUNT_2_BIT)) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

bool Renderer::create_render_pass() {
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;

    // Attachment 0: color the subpass renders into (multisampled when MSAA is
    // on, otherwise the swapchain image directly).
    VkAttachmentDescription color{};
    color.format = color_format_;
    color.samples = samples_;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // With MSAA the multisampled color is resolved and never stored; without
    // MSAA this IS the swapchain image and must be stored.
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                         : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = depth_format_;
    depth.samples = samples_;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // Attachment 2 (MSAA only): the single-sampled swapchain image the
    // multisampled color resolves into. The XR runtime expects it in
    // COLOR_ATTACHMENT_OPTIMAL and handles the present transition itself.
    VkAttachmentDescription resolve{};
    resolve.format = color_format_;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolve.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve_ref{2,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    if (msaa) sub.pResolveAttachments = &resolve_ref;

    std::array<VkAttachmentDescription, 3> atts{color, depth, resolve};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = msaa ? 3u : 2u;
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    return vkCreateRenderPass(vk_->device(), &ci, nullptr, &render_pass_) ==
           VK_SUCCESS;
}

bool Renderer::ensure_joint_buffer(std::uint32_t matrix_capacity) {
    if (matrix_capacity <= joint_ssbo_capacity_ && joint_ssbo_) return true;

    // Grow in powers of two so a scene that adds skinned characters over time
    // reallocates a handful of times, not once per new character.
    std::uint32_t cap = std::max<std::uint32_t>(joint_ssbo_capacity_, 256u);
    while (cap < matrix_capacity) cap *= 2u;

    VkBuffer old_buf = joint_ssbo_;
    VmaAllocation old_alloc = joint_ssbo_alloc_;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = static_cast<VkDeviceSize>(cap) * sizeof(Mat4);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ainfo{};
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &buf, &alloc, &ainfo) !=
        VK_SUCCESS) {
        PTV_ERROR("failed to allocate joint palette buffer ({} matrices)", cap);
        return false;
    }

    joint_ssbo_ = buf;
    joint_ssbo_alloc_ = alloc;
    joint_ssbo_mapped_ = ainfo.pMappedData;
    joint_ssbo_capacity_ = cap;

    // Point the descriptor at the new buffer. Callers only reach here either
    // before the first frame or right after the frame fence, so no in-flight
    // command buffer is reading the old one.
    if (env_set_ != VK_NULL_HANDLE) {
        VkDescriptorBufferInfo dbi{joint_ssbo_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = env_set_;
        w.dstBinding = 1;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(vk_->device(), 1, &w, 0, nullptr);
    }

    if (old_buf) {
        vmaDestroyBuffer(vk_->allocator(), old_buf, old_alloc);
    }
    return true;
}

void Renderer::begin_joint_palettes() { joint_palettes_.clear(); }

int Renderer::push_joint_palette(const Mat4* matrices, int count) {
    if (!matrices || count <= 0) return -1;
    const int base = static_cast<int>(joint_palettes_.size());
    joint_palettes_.insert(joint_palettes_.end(), matrices, matrices + count);
    return base;
}

bool Renderer::create_descriptors() {
    // Set 0, binding 0: environment/lighting UBO (fragment stage).
    // Set 0, binding 1: joint matrix palettes (vertex stage). One set serves
    // both the static and skinned pipelines; the static vertex shader simply
    // never reads binding 1.
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(vk_->device(), &lci, nullptr,
                                    &env_set_layout_) != VK_SUCCESS) {
        return false;
    }

    // Persistently-mapped host-visible UBO, refreshed once per frame.
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(GpuEnv);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ainfo{};
    if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &env_ubo_, &env_ubo_alloc_,
                        &ainfo) != VK_SUCCESS) {
        return false;
    }
    env_ubo_mapped_ = ainfo.pMappedData;

    VkDescriptorPoolSize ps[2]{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(vk_->device(), &pci, nullptr, &desc_pool_) !=
        VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo dsi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = desc_pool_;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &env_set_layout_;
    if (vkAllocateDescriptorSets(vk_->device(), &dsi, &env_set_) != VK_SUCCESS) {
        return false;
    }

    // Binding 1 must reference a real buffer even in a scene with no skinned
    // meshes at all: the layout declares it, so leaving it unwritten is a
    // validation error the moment anything binds this set.
    if (!ensure_joint_buffer(256u)) return false;

    VkDescriptorBufferInfo dbi{env_ubo_, 0, sizeof(GpuEnv)};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = env_set_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(vk_->device(), 1, &w, 0, nullptr);
    return true;
}

bool Renderer::create_pipeline() {
    const std::string base = std::string(PTV_SHADER_DIR);
    vs_ = make_shader(vk_->device(), base + "/scene.vert.spv");
    fs_ = make_shader(vk_->device(), base + "/scene.frag.spv");
    if (!vs_ || !fs_) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbd{};
    vbd.binding = 0;
    vbd.stride = sizeof(Vertex);
    vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[3]{};
    vattr[0].location = 0;
    vattr[0].binding = 0;
    vattr[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vattr[0].offset = offsetof(Vertex, pos);
    vattr[1].location = 1;
    vattr[1].binding = 0;
    vattr[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vattr[1].offset = offsetof(Vertex, normal);
    // Per-vertex baseColor (RGBA, linear). For GLB meshes this carries the
    // material's baseColorFactor (and COLOR_0 if present); for the
    // procedural cube path it's white so the per-DrawItem tint dominates.
    vattr[2].location = 2;
    vattr[2].binding = 0;
    vattr[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[2].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &env_set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(vk_->device(), &plci, nullptr,
                               &pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    VkGraphicsPipelineCreateInfo gpci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipeline_layout_;
    gpci.renderPass = render_pass_;
    gpci.subpass = 0;

    // A mirrored variant, identical but for the cull mode. Reflecting an
    // object across a plane is an improper transform, so its triangles come
    // out wound the other way; drawing them with the usual back-face culling
    // would show the inside of the avatar. Two pipelines is the portable
    // answer -- dynamic cull state needs an extension we would then have to
    // gate on, and Quest driver support for it varies.
    VkPipelineRasterizationStateCreateInfo rs_mirror = rs;
    rs_mirror.cullMode = VK_CULL_MODE_FRONT_BIT;
    VkGraphicsPipelineCreateInfo gpci_mirror = gpci;
    gpci_mirror.pRasterizationState = &rs_mirror;

    const VkGraphicsPipelineCreateInfo infos[2]{gpci, gpci_mirror};
    VkPipeline out[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 2, infos,
                                  nullptr, out) != VK_SUCCESS) {
        return false;
    }
    pipeline_ = out[0];
    pipeline_mirror_ = out[1];
    return true;
}

bool Renderer::create_skinned_pipeline() {
    // Everything except the vertex shader and the vertex input is identical to
    // create_pipeline, including the pipeline layout -- so the two pipelines
    // share push constants and the same descriptor set, and switching between
    // them costs only a vkCmdBindPipeline.
    //
    // Static meshes deliberately do NOT carry joints/weights. On a
    // bandwidth-bound tile GPU, widening every vertex in the scene by 8 bytes
    // to serve the handful that are rigged is the wrong trade.
    const std::string base = std::string(PTV_SHADER_DIR);
    skinned_vs_ = make_shader(vk_->device(), base + "/scene_skinned.vert.spv");
    if (!skinned_vs_ || !fs_) return false;
    if (pipeline_layout_ == VK_NULL_HANDLE) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = skinned_vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";

    using gltf_mesh_load::SkinnedVertex;
    VkVertexInputBindingDescription vbd{};
    vbd.binding = 0;
    vbd.stride = sizeof(SkinnedVertex);
    vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[5]{};
    vattr[0].location = 0;
    vattr[0].binding = 0;
    vattr[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vattr[0].offset = offsetof(SkinnedVertex, pos);
    vattr[1].location = 1;
    vattr[1].binding = 0;
    vattr[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vattr[1].offset = offsetof(SkinnedVertex, normal);
    vattr[2].location = 2;
    vattr[2].binding = 0;
    vattr[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vattr[2].offset = offsetof(SkinnedVertex, color);
    // Joint indices stay integral (_UINT, read as uvec4); weights are
    // _UNORM so the hardware hands the shader the 0..1 partition directly.
    vattr[3].location = 3;
    vattr[3].binding = 0;
    vattr[3].format = VK_FORMAT_R8G8B8A8_UINT;
    vattr[3].offset = offsetof(SkinnedVertex, joints);
    vattr[4].location = 4;
    vattr[4].binding = 0;
    vattr[4].format = VK_FORMAT_R8G8B8A8_UNORM;
    vattr[4].offset = offsetof(SkinnedVertex, weights);

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = pipeline_layout_;
    gpci.renderPass = render_pass_;
    gpci.subpass = 0;

    // Mirrored variant, same reasoning as the static pipeline above.
    VkPipelineRasterizationStateCreateInfo rs_mirror = rs;
    rs_mirror.cullMode = VK_CULL_MODE_FRONT_BIT;
    VkGraphicsPipelineCreateInfo gpci_mirror = gpci;
    gpci_mirror.pRasterizationState = &rs_mirror;

    const VkGraphicsPipelineCreateInfo infos[2]{gpci, gpci_mirror};
    VkPipeline out[2]{VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 2, infos,
                                  nullptr, out) != VK_SUCCESS) {
        return false;
    }
    skinned_pipeline_ = out[0];
    skinned_pipeline_mirror_ = out[1];
    return true;
}

bool Renderer::create_sky_pipeline() {
    const std::string base = std::string(PTV_SHADER_DIR);
    sky_vs_ = make_shader(vk_->device(), base + "/sky.vert.spv");
    sky_fs_ = make_shader(vk_->device(), base + "/sky.frag.spv");
    if (!sky_vs_ || !sky_fs_) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sky_vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = sky_fs_;
    stages[1].pName = "main";

    // No vertex input: the vertex shader emits a fullscreen triangle from
    // gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;

    // Sky fills the background: no depth test, no depth writes (so geometry
    // drawn afterwards always wins).
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(SkyPush);

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &env_set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(vk_->device(), &plci, nullptr,
                               &sky_pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    VkGraphicsPipelineCreateInfo gpci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = sky_pipeline_layout_;
    gpci.renderPass = render_pass_;
    gpci.subpass = 0;
    return vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &gpci,
                                     nullptr, &sky_pipeline_) == VK_SUCCESS;
}

bool Renderer::create_vignette_pipeline() {
    const std::string base = std::string(PTV_SHADER_DIR);
    vignette_fs_ = make_shader(vk_->device(), base + "/vignette.frag.spv");
    if (!sky_vs_ || !vignette_fs_) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sky_vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vignette_fs_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;
    // Overlay on top of everything: no depth test, no depth writes.
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(VignettePush);
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(vk_->device(), &plci, nullptr,
                               &vignette_pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    VkGraphicsPipelineCreateInfo gpci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = vignette_pipeline_layout_;
    gpci.renderPass = render_pass_;
    gpci.subpass = 0;
    return vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &gpci,
                                     nullptr, &vignette_pipeline_) == VK_SUCCESS;
}

int Renderer::create_mesh_slot(const std::vector<gltf_mesh_load::LitVertex>& verts,
                               const std::vector<std::uint32_t>& indices) {
    return create_mesh_slot_impl(verts.data(), verts.size(), sizeof(verts[0]),
                                 indices.data(), indices.size(),
                                 /*skinned=*/false);
}

int Renderer::create_mesh_slot_skinned(
    const std::vector<gltf_mesh_load::SkinnedVertex>& verts,
    const std::vector<std::uint32_t>& indices) {
    return create_mesh_slot_impl(verts.data(), verts.size(), sizeof(verts[0]),
                                 indices.data(), indices.size(),
                                 /*skinned=*/true);
}

int Renderer::create_mesh_slot_impl(const void* verts, std::size_t vert_count,
                                    std::size_t vert_stride,
                                    const std::uint32_t* indices,
                                    std::size_t index_count, bool skinned) {
    // Both vertex structs lead with float pos[3], which is all the bounding
    // sphere below needs; the static_asserts keep that true if either layout
    // is ever reordered.
    static_assert(offsetof(gltf_mesh_load::LitVertex, pos) == 0,
                  "LitVertex must start with pos[3]");
    static_assert(offsetof(gltf_mesh_load::SkinnedVertex, pos) == 0,
                  "SkinnedVertex must start with pos[3]");

    if (!vk_ || !verts || !indices || vert_count == 0 || index_count == 0)
        return -1;
    auto vertex_pos = [&](std::size_t i) -> const float* {
        return reinterpret_cast<const float*>(
            static_cast<const std::uint8_t*>(verts) + i * vert_stride);
    };
    auto make_vma_buffer = [&](VkBufferUsageFlags usage, VkDeviceSize size,
                               VkBuffer& buf, VmaAllocation& alloc) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        return vmaCreateBuffer(vk_->allocator(), &bci, &aci, &buf, &alloc,
                               nullptr) == VK_SUCCESS;
    };
    GltfGpuMesh gm{};
    gm.skinned = skinned;
    const VkDeviceSize vb_size =
        static_cast<VkDeviceSize>(vert_count * vert_stride);
    const VkDeviceSize ib_size =
        static_cast<VkDeviceSize>(index_count * sizeof(std::uint32_t));
    if (!make_vma_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vb_size, gm.vb,
                         gm.vb_alloc)) {
        return -1;
    }
    upload_buffer(*vk_, gm.vb, gm.vb_alloc, verts,
                  static_cast<size_t>(vb_size));
    if (!make_vma_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ib_size, gm.ib,
                         gm.ib_alloc)) {
        vmaDestroyBuffer(vk_->allocator(), gm.vb, gm.vb_alloc);
        return -1;
    }
    upload_buffer(*vk_, gm.ib, gm.ib_alloc, indices,
                  static_cast<size_t>(ib_size));
    gm.index_count = static_cast<uint32_t>(index_count);
    gm.triangle_count = gm.index_count / 3;

    // Bounding sphere in mesh space, used for frustum + distance culling.
    // Centre-of-AABB + max radius is tighter than an AABB-corner radius and is
    // cheap enough to compute once at upload.
    //
    // For a skinned mesh this is the BIND-POSE bound. A pose can push vertices
    // outside it, so animated characters are culled slightly optimistically;
    // that is the standard trade and far cheaper than re-deriving bounds per
    // frame from the palette.
    float mn[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float mx[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (std::size_t i = 0; i < vert_count; ++i) {
        const float* p = vertex_pos(i);
        for (int c = 0; c < 3; ++c) {
            if (p[c] < mn[c]) mn[c] = p[c];
            if (p[c] > mx[c]) mx[c] = p[c];
        }
    }
    const float cx = 0.5f * (mn[0] + mx[0]);
    const float cy = 0.5f * (mn[1] + mx[1]);
    const float cz = 0.5f * (mn[2] + mx[2]);
    float r2 = 0.0f;
    for (std::size_t i = 0; i < vert_count; ++i) {
        const float* p = vertex_pos(i);
        const float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2) r2 = d2;
    }
    gm.local_radius = std::sqrt(r2);

    resident_triangles_ += gm.triangle_count;

    // Reuse a slot freed by eviction before growing the table, so mesh_slot
    // indices stay stable and the vector does not grow without bound.
    if (!free_mesh_slots_.empty()) {
        const int slot = free_mesh_slots_.back();
        free_mesh_slots_.pop_back();
        gltf_meshes_[static_cast<size_t>(slot)] = gm;
        return slot;
    }
    const int slot = static_cast<int>(gltf_meshes_.size());
    gltf_meshes_.push_back(gm);
    return slot;
}

// ---------------------------------------------------------------------------
// Asynchronous mesh streaming
// ---------------------------------------------------------------------------

void Renderer::start_stream_worker() {
    if (stream_thread_.joinable()) return;
    stream_stop_ = false;
    stream_thread_ = std::thread([this] { stream_worker_main(); });
}

void Renderer::stop_stream_worker() {
    if (!stream_thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        stream_stop_ = true;
    }
    stream_cv_.notify_all();
    stream_thread_.join();
    stream_queue_.clear();
    stream_done_.clear();
    stream_inflight_.clear();
}

void Renderer::stream_worker_main() {
    for (;;) {
        StreamRequest req;
        {
            std::unique_lock<std::mutex> lock(stream_mutex_);
            stream_cv_.wait(lock, [this] {
                return stream_stop_.load() || !stream_queue_.empty();
            });
            if (stream_stop_.load()) return;

            // Pop the highest-priority (nearest) request. The queue is small
            // (bounded by scene asset count), so a linear scan beats keeping a
            // heap in sync as priorities change every frame.
            size_t best = 0;
            for (size_t i = 1; i < stream_queue_.size(); ++i) {
                if (stream_queue_[i].priority < stream_queue_[best].priority) {
                    best = i;
                }
            }
            req = stream_queue_[best];
            stream_queue_.erase(stream_queue_.begin() +
                                static_cast<std::ptrdiff_t>(best));
        }

        StreamResult result;
        result.key = req.key;
        std::string err;
        result.ok = gltf_mesh_load::load_glb_merged(
            req.path, req.target_scale_meters, result.verts, result.indices,
            err, req.unit_scale);
        if (!result.ok) {
            PTV_WARN("stream: GLB load failed ({}): {}", req.path, err);
        }
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            stream_done_.push_back(std::move(result));
        }
    }
}

std::string Renderer::mesh_key(const std::string& path, float target_scale,
                               float unit_scale) {
    char buf[40];
    if (unit_scale > 1e-8f) {
        std::snprintf(buf, sizeof(buf), "|u%.7g", unit_scale);
    } else {
        std::snprintf(buf, sizeof(buf), "|t%.5g", target_scale);
    }
    return path + buf;
}

int Renderer::resident_slot(const std::string& key) {
    auto it = gltf_path_to_slot_.find(key);
    if (it == gltf_path_to_slot_.end()) return -1;
    gltf_meshes_[static_cast<size_t>(it->second)].last_used_frame = frame_index_;
    return it->second;
}

void Renderer::request_stream(const std::string& path,
                              float target_scale_meters, float unit_scale,
                              float priority) {
    if (path.empty()) return;
    const std::string key = mesh_key(path, target_scale_meters, unit_scale);
    if (gltf_path_to_slot_.count(key)) return;
    if (stream_failed_.count(key)) return;  // don't retry a broken asset forever

    std::lock_guard<std::mutex> lock(stream_mutex_);
    if (stream_inflight_.count(key)) {
        // Already queued: keep the most urgent priority seen this frame so the
        // nearest object still wins if the player turns around.
        for (auto& q : stream_queue_) {
            if (q.key == key) {
                q.priority = std::min(q.priority, priority);
                break;
            }
        }
        return;
    }
    stream_inflight_.insert(key);
    stream_queue_.push_back(StreamRequest{path, key, target_scale_meters,
                                          unit_scale, priority});
    stream_cv_.notify_one();
}

void Renderer::pump_streaming() {
    ++frame_index_;
    if (!vk_) return;

    // Upload at most a couple of meshes per frame. Meta's own open-world
    // streaming sample throttles to ~4 async operations per frame for exactly
    // this reason: a burst of uploads is what turns into a visible hitch (and,
    // on Android, a blocked event pump -> ANR).
    constexpr int kMaxUploadsPerFrame = 2;
    constexpr double kMaxUploadMillis = 4.0;

    const auto start = std::chrono::steady_clock::now();
    int uploaded = 0;
    while (uploaded < kMaxUploadsPerFrame) {
        StreamResult result;
        {
            std::lock_guard<std::mutex> lock(stream_mutex_);
            if (stream_done_.empty()) break;
            result = std::move(stream_done_.back());
            stream_done_.pop_back();
            stream_inflight_.erase(result.key);
        }

        if (result.ok) {
            const int slot = create_mesh_slot(result.verts, result.indices);
            if (slot >= 0) {
                auto& gm = gltf_meshes_[static_cast<size_t>(slot)];
                gm.path = result.key;
                gm.evictable = true;
                gm.last_used_frame = frame_index_;
                gltf_path_to_slot_[result.key] = slot;
            } else {
                stream_failed_.insert(result.key);
            }
        } else {
            stream_failed_.insert(result.key);
        }
        ++uploaded;

        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start)
                .count();
        if (elapsed_ms >= kMaxUploadMillis) break;
    }

    evict_to_budget();

    std::lock_guard<std::mutex> lock(stream_mutex_);
    stats_.streaming_pending =
        static_cast<std::uint32_t>(stream_inflight_.size());
}

void Renderer::destroy_mesh_slot(GltfGpuMesh& gm) {
    if (!vk_) return;
    if (gm.vb) vmaDestroyBuffer(vk_->allocator(), gm.vb, gm.vb_alloc);
    if (gm.ib) vmaDestroyBuffer(vk_->allocator(), gm.ib, gm.ib_alloc);
    gm = GltfGpuMesh{};
}

void Renderer::evict_to_budget() {
    if (!perf_.evict_when_over_budget) return;
    if (resident_triangles_ <= perf_.resident_triangle_budget) return;

    // Safe to free here: draw_frame waits on its fence before returning, so no
    // command buffer is still referencing these buffers. Evict at most one per
    // frame to spread the cost (same rationale as the upload throttle).
    int victim = -1;
    std::uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < gltf_meshes_.size(); ++i) {
        const auto& gm = gltf_meshes_[i];
        if (!gm.evictable || gm.vb == VK_NULL_HANDLE) continue;
        if (gm.last_used_frame >= frame_index_) continue;  // in use this frame
        if (gm.last_used_frame < oldest) {
            oldest = gm.last_used_frame;
            victim = static_cast<int>(i);
        }
    }
    if (victim < 0) return;

    auto& gm = gltf_meshes_[static_cast<size_t>(victim)];
    resident_triangles_ -= std::min(resident_triangles_, gm.triangle_count);
    gltf_path_to_slot_.erase(gm.path);
    PTV_INFO("stream: evicted {} ({} tris, resident now {})", gm.path,
             gm.triangle_count, resident_triangles_);
    destroy_mesh_slot(gm);
    free_mesh_slots_.push_back(victim);
}

void Renderer::set_performance(const ScenePerformance& perf) {
    perf_ = perf;
    cull_distance_m_ = perf.cull_distance_m;
    lod_bias_ = 0.0f;
}

bool Renderer::create_primitive_meshes() {
    std::vector<gltf_mesh_load::LitVertex> v;
    std::vector<uint32_t> idx;

    v.clear(); idx.clear(); make_sphere(v, idx);
    prim_slot_sphere_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_cylinder(v, idx);
    prim_slot_cylinder_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_plane(v, idx);
    prim_slot_plane_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_ramp(v, idx);
    prim_slot_ramp_ = create_mesh_slot(v, idx);

    return prim_slot_sphere_ >= 0 && prim_slot_cylinder_ >= 0 &&
           prim_slot_plane_ >= 0 && prim_slot_ramp_ >= 0;
}

bool Renderer::create_msaa_color_image() {
    if (samples_ == VK_SAMPLE_COUNT_1_BIT) return true;  // no MSAA target needed
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = color_format_;
    ici.extent = {eye_width_, eye_height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = samples_;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vk_->allocator(), &ici, &aci, &msaa_color_image_,
                       &msaa_color_alloc_, nullptr) != VK_SUCCESS) {
        PTV_ERROR("vmaCreateImage(msaa color) failed");
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = msaa_color_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    return vkCreateImageView(vk_->device(), &vci, nullptr, &msaa_color_view_) ==
           VK_SUCCESS;
}

bool Renderer::create_cube_mesh() {
    auto make_vma_buffer = [&](VkBufferUsageFlags usage, VkDeviceSize size,
                               VkBuffer& buf, VmaAllocation& alloc) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        return vmaCreateBuffer(vk_->allocator(), &bci, &aci, &buf, &alloc,
                               nullptr) == VK_SUCCESS;
    };

    if (!make_vma_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         sizeof(kCubeVerts), cube_vb_, cube_vb_alloc_))
        return false;
    upload_buffer(*vk_, cube_vb_, cube_vb_alloc_, kCubeVerts.data(),
                  sizeof(kCubeVerts));

    if (!make_vma_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         sizeof(kCubeIndices), cube_ib_, cube_ib_alloc_))
        return false;
    upload_buffer(*vk_, cube_ib_, cube_ib_alloc_, kCubeIndices.data(),
                  sizeof(kCubeIndices));
    cube_index_count_ = static_cast<uint32_t>(kCubeIndices.size());
    return true;
}

bool Renderer::create_depth_image() {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depth_format_;
    ici.extent = {eye_width_, eye_height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = samples_;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vk_->allocator(), &ici, &aci, &depth_image_,
                       &depth_alloc_, nullptr) != VK_SUCCESS) {
        PTV_ERROR("vmaCreateImage(depth) failed");
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depth_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depth_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    return vkCreateImageView(vk_->device(), &vci, nullptr, &depth_view_) ==
           VK_SUCCESS;
}

bool Renderer::create_command_resources() {
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vk_->graphics_queue_family();
    if (vkCreateCommandPool(vk_->device(), &cpi, nullptr, &cmd_pool_) !=
        VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk_->device(), &ai, &cmd_) != VK_SUCCESS) {
        return false;
    }
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return vkCreateFence(vk_->device(), &fci, nullptr, &frame_fence_) ==
           VK_SUCCESS;
}

VkImageView Renderer::get_or_create_color_view(VkImage img) {
    auto it = color_views_.find(img);
    if (it != color_views_.end()) return it->second;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vk_->device(), &vci, nullptr, &v) != VK_SUCCESS) {
        PTV_ERROR("vkCreateImageView (swapchain color) failed");
        return VK_NULL_HANDLE;
    }
    color_views_.emplace(img, v);
    return v;
}

VkFramebuffer Renderer::get_or_create_framebuffer(VkImage img) {
    auto it = framebuffers_.find(img);
    if (it != framebuffers_.end()) return it->second;
    VkImageView swap_v = get_or_create_color_view(img);
    if (!swap_v) return VK_NULL_HANDLE;

    // MSAA: [msaa_color, depth, resolve(swapchain)]. Non-MSAA:
    // [swapchain, depth]. The MSAA color + depth are shared across eyes; only
    // the resolve target (swapchain image) differs per image.
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    std::array<VkImageView, 3> atts{};
    uint32_t att_count = 0;
    if (msaa) {
        atts[0] = msaa_color_view_;
        atts[1] = depth_view_;
        atts[2] = swap_v;
        att_count = 3;
    } else {
        atts[0] = swap_v;
        atts[1] = depth_view_;
        att_count = 2;
    }
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = render_pass_;
    fci.attachmentCount = att_count;
    fci.pAttachments = atts.data();
    fci.width = eye_width_;
    fci.height = eye_height_;
    fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vk_->device(), &fci, nullptr, &fb) != VK_SUCCESS) {
        PTV_ERROR("vkCreateFramebuffer failed");
        return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(img, fb);
    return fb;
}

void Renderer::draw_frame(const std::array<EyeTarget, 2>& eyes,
                          const std::vector<DrawItem>& items,
                          const RenderEnv& env) {
    // Wait for the PREVIOUS frame's GPU work to finish before touching the
    // command buffer or the swapchain-image framebuffers again. The fence is
    // created signaled so the very first frame returns immediately.
    vkWaitForFences(vk_->device(), 1, &frame_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(vk_->device(), 1, &frame_fence_);

    // ---- Update the environment/lighting UBO (shared by both eyes) --------
    // We are past the fence wait, so the previous frame is done reading it and
    // it is safe to overwrite the persistently-mapped buffer.
    if (env_ubo_mapped_) {
        Vec3 cam{0.0f};
        for (const auto& e : eyes) {
            Mat4 inv = glm::inverse(e.view);
            cam += Vec3(inv[3]);
        }
        cam *= 0.5f;

        Vec3 sun = env.sun_direction;
        if (glm::length(sun) < 1e-5f) sun = Vec3{0.0f, -1.0f, 0.0f};
        sun = glm::normalize(sun);

        GpuEnv g{};
        g.sun_dir[0] = sun.x; g.sun_dir[1] = sun.y; g.sun_dir[2] = sun.z;
        g.sun_color[0] = env.sun_color.x; g.sun_color[1] = env.sun_color.y;
        g.sun_color[2] = env.sun_color.z; g.sun_color[3] = env.sun_intensity;
        g.sky_top[0] = env.sky_top.x; g.sky_top[1] = env.sky_top.y;
        g.sky_top[2] = env.sky_top.z;
        g.sky_top[3] = env.exposure;  // global pre-tonemap brightness multiplier
        // sky_horizon doubles as both the sky-gradient horizon and the fog
        // color; .w carries fog density.
        g.sky_horizon[0] = env.sky_horizon.x; g.sky_horizon[1] = env.sky_horizon.y;
        g.sky_horizon[2] = env.sky_horizon.z; g.sky_horizon[3] = env.fog_density;
        g.ambient[0] = env.ambient_ground.x; g.ambient[1] = env.ambient_ground.y;
        g.ambient[2] = env.ambient_ground.z; g.ambient[3] = env.ambient_intensity;
        const int count =
            std::min<int>(kMaxPointLights, static_cast<int>(env.points.size()));
        g.camera_pos[0] = cam.x; g.camera_pos[1] = cam.y; g.camera_pos[2] = cam.z;
        g.camera_pos[3] = static_cast<float>(count);
        for (int i = 0; i < count; ++i) {
            const PointLight& p = env.points[static_cast<size_t>(i)];
            g.points[i].pos_range[0] = p.position.x;
            g.points[i].pos_range[1] = p.position.y;
            g.points[i].pos_range[2] = p.position.z;
            g.points[i].pos_range[3] = p.range;
            g.points[i].color_intensity[0] = p.color.x;
            g.points[i].color_intensity[1] = p.color.y;
            g.points[i].color_intensity[2] = p.color.z;
            g.points[i].color_intensity[3] = p.intensity;
        }
        std::memcpy(env_ubo_mapped_, &g, sizeof(g));
        // Make the write visible to the device before submit. No-op on the
        // coherent host memory VMA usually picks; correct if it doesn't.
        vmaFlushAllocation(vk_->allocator(), env_ubo_alloc_, 0, sizeof(g));
    }

    // ---- Upload this frame's joint palettes (shared by both eyes) ---------
    // Palettes are world-space and therefore view-independent, so this runs
    // once here rather than per eye. Growing the buffer is safe at this point
    // for the same reason the env UBO write is: we are past the fence, so no
    // command buffer is still reading the old allocation.
    if (!joint_palettes_.empty()) {
        if (ensure_joint_buffer(
                static_cast<std::uint32_t>(joint_palettes_.size())) &&
            joint_ssbo_mapped_) {
            const size_t bytes = joint_palettes_.size() * sizeof(Mat4);
            std::memcpy(joint_ssbo_mapped_, joint_palettes_.data(), bytes);
            vmaFlushAllocation(vk_->allocator(), joint_ssbo_alloc_, 0, bytes);
        }
    }

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    for (const auto& eye : eyes) {
        VkFramebuffer fb = get_or_create_framebuffer(eye.image);
        if (!fb) continue;

        // 3 clear values so the index is valid whether or not MSAA adds the
        // resolve attachment (the resolve slot's clear value is ignored).
        std::array<VkClearValue, 3> clears{};
        clears[0].color = {{env.sky_horizon.x, env.sky_horizon.y,
                            env.sky_horizon.z, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        clears[2].color = clears[0].color;
        const uint32_t clear_count =
            samples_ != VK_SAMPLE_COUNT_1_BIT ? 3u : 2u;

        VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpi.renderPass = render_pass_;
        rpi.framebuffer = fb;
        rpi.renderArea.offset = {eye.offset_x, eye.offset_y};
        rpi.renderArea.extent = {eye.width, eye.height};
        rpi.clearValueCount = clear_count;
        rpi.pClearValues = clears.data();
        vkCmdBeginRenderPass(cmd_, &rpi, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{};
        vp.x = static_cast<float>(eye.offset_x);
        vp.y = static_cast<float>(eye.offset_y + static_cast<int32_t>(eye.height));
        vp.width = static_cast<float>(eye.width);
        vp.height = -static_cast<float>(eye.height);  // flip Y for Vulkan
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd_, 0, 1, &vp);

        VkRect2D sc{{eye.offset_x, eye.offset_y}, {eye.width, eye.height}};
        vkCmdSetScissor(cmd_, 0, 1, &sc);

        // ---- Sky pass: gradient + sun fills the background --------------
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                sky_pipeline_layout_, 0, 1, &env_set_, 0,
                                nullptr);
        SkyPush sky_pc{};
        sky_pc.inv_view_proj = glm::inverse(eye.projection * eye.view);
        vkCmdPushConstants(cmd_, sky_pipeline_layout_,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sky_pc),
                           &sky_pc);
        vkCmdDraw(cmd_, 3, 1, 0, 0);

        // ---- Geometry pass ----------------------------------------------
        // Both pipelines share pipeline_layout_ and env_set_, so the
        // descriptor set is bound once and only vkCmdBindPipeline changes
        // when an item switches between static and skinned geometry.
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &env_set_, 0, nullptr);
        VkPipeline bound = VK_NULL_HANDLE;

        Mat4 vp_mat = eye.projection * eye.view;
        for (const auto& it : items) {
            const GltfGpuMesh* gm =
                (it.mesh_slot >= 0 &&
                 static_cast<size_t>(it.mesh_slot) < gltf_meshes_.size())
                    ? &gltf_meshes_[static_cast<size_t>(it.mesh_slot)]
                    : nullptr;

            // Only skin when the item asked for it AND the mesh really is
            // skinned: drawing 40-byte vertices through the skinned pipeline
            // would read joint indices out of the next vertex's position.
            const bool skinned = gm && gm->skinned && it.joint_base >= 0 &&
                                 it.joint_count > 0;
            if (gm && gm->skinned && !skinned) continue;

            VkPipeline want =
                it.mirrored ? (skinned ? skinned_pipeline_mirror_
                                       : pipeline_mirror_)
                            : (skinned ? skinned_pipeline_ : pipeline_);
            if (want != bound) {
                vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
                bound = want;
            }

            PushConstants pc{};
            pc.view_proj = vp_mat;
            pc.model = it.model;
            pc.color = it.color;
            pc.params = Vec4{it.material_id,
                             skinned ? static_cast<float>(it.joint_base) : 0.0f,
                             0.0f, 0.0f};
            vkCmdPushConstants(cmd_, pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);

            if (gm) {
                VkDeviceSize voff = 0;
                vkCmdBindVertexBuffers(cmd_, 0, 1, &gm->vb, &voff);
                vkCmdBindIndexBuffer(cmd_, gm->ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd_, gm->index_count, 1, 0, 0, 0);
            } else {
                VkDeviceSize voff = 0;
                vkCmdBindVertexBuffers(cmd_, 0, 1, &cube_vb_, &voff);
                vkCmdBindIndexBuffer(cmd_, cube_ib_, 0, VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cmd_, cube_index_count_, 1, 0, 0, 0);
            }
        }

        // ---- Comfort vignette: last, over everything --------------------
        if (vignette_pipeline_ != VK_NULL_HANDLE && vignette_opacity_ > 0.002f) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              vignette_pipeline_);
            constexpr float kDeg = 3.14159265f / 180.0f;
            VignettePush vpc{};
            vpc.inv_proj = glm::inverse(eye.projection);
            vpc.params = Vec4(vignette_clear_deg_ * kDeg,
                              vignette_feather_deg_ * kDeg,
                              std::clamp(vignette_opacity_, 0.0f, 1.0f), 0.0f);
            vpc.color = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            vkCmdPushConstants(cmd_, vignette_pipeline_layout_,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(vpc),
                               &vpc);
            vkCmdDraw(cmd_, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd_);
    }
    vkEndCommandBuffer(cmd_);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, frame_fence_);

    // IMPORTANT: Block until our submit has fully completed on the GPU
    // BEFORE returning to the XR runtime. The caller (XrRuntime::render_frame)
    // will call xrReleaseSwapchainImage immediately after we return, which
    // hands the eye images back to the OpenXR compositor. If the GPU hasn't
    // finished writing them yet, the compositor samples from images that are
    // still in the middle of a render pass -- on Meta Quest Link this reads
    // as full-white "flash" frames in the headset, and the compositor's own
    // submits on our shared graphics queue race with our pending command
    // buffer (producing VUID-vkQueueSubmit-pCommandBuffers-00071 validation
    // errors).
    //
    // Waiting here serializes us to one frame in flight. That is fine for
    // the starter room (and any scene we currently generate) and is the
    // pattern the Khronos hello_xr sample uses for the single-queue case.
    vkWaitForFences(vk_->device(), 1, &frame_fence_, VK_TRUE, UINT64_MAX);
}

void Renderer::destroy_gltf_meshes() {
    if (!vk_) return;
    for (auto& m : gltf_meshes_) {
        if (m.vb) vmaDestroyBuffer(vk_->allocator(), m.vb, m.vb_alloc);
        if (m.ib) vmaDestroyBuffer(vk_->allocator(), m.ib, m.ib_alloc);
        m.vb = VK_NULL_HANDLE;
        m.ib = VK_NULL_HANDLE;
        m.vb_alloc = m.ib_alloc = nullptr;
        m.index_count = 0;
    }
    gltf_meshes_.clear();
    gltf_path_to_slot_.clear();
    inline_mesh_slots_.clear();
    free_mesh_slots_.clear();
    entity_lod_.clear();
    resident_triangles_ = 0;
    // Built-in primitive meshes lived in gltf_meshes_; their slots are now
    // invalid.
    prim_slot_sphere_ = prim_slot_cylinder_ = prim_slot_plane_ =
        prim_slot_ramp_ = -1;
}

int Renderer::load_glb(const std::string& resolved_path,
                       float target_scale_meters) {
    return ensure_gltf_mesh(resolved_path, target_scale_meters);
}

bool Renderer::mesh_slot_is_skinned(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= gltf_meshes_.size())
        return false;
    return gltf_meshes_[static_cast<size_t>(slot)].skinned;
}

int Renderer::load_glb_skinned(const std::string& resolved_path,
                               float target_scale_meters,
                               gltf_mesh_load::SkeletonData& out_skeleton) {
    if (!vk_ || resolved_path.empty()) return -1;
    if (!asset_source().exists(resolved_path)) {
        PTV_WARN("skinned GLB not found via asset source: {}", resolved_path);
        return -1;
    }
    // Cached slots are keyed by path just like the static ones, but a skinned
    // hit still has to refill the caller's skeleton, so the parse is repeated.
    // Skeletons are tiny next to geometry and this runs at scene load, not
    // per frame.
    std::vector<gltf_mesh_load::SkinnedVertex> verts;
    std::vector<std::uint32_t> indices;
    std::string err;
    if (!gltf_mesh_load::load_glb_skinned(resolved_path, target_scale_meters,
                                          verts, indices, out_skeleton, err)) {
        PTV_WARN("skinned GLB load failed ({}): {}", resolved_path, err);
        return -1;
    }

    const std::string key = mesh_key(resolved_path, target_scale_meters, 0.0f);
    auto it = gltf_path_to_slot_.find(key);
    if (it != gltf_path_to_slot_.end()) {
        gltf_meshes_[static_cast<size_t>(it->second)].last_used_frame =
            frame_index_;
        return it->second;
    }

    const int slot = create_mesh_slot_skinned(verts, indices);
    if (slot < 0) return -1;
    auto& gm = gltf_meshes_[static_cast<size_t>(slot)];
    gm.path = key;
    gm.last_used_frame = frame_index_;
    // Not evictable: the skeleton on the entity would outlive the geometry and
    // the entity would silently stop drawing with no way to recover it.
    gm.evictable = false;
    gltf_path_to_slot_[key] = slot;
    return slot;
}

int Renderer::ensure_gltf_mesh(const std::string& resolved_path,
                               float target_scale_meters) {
    if (!vk_ || resolved_path.empty()) return -1;
    // Resolve through the process-global asset source (filesystem on desktop,
    // AAssetManager inside the APK on Quest) -- a raw std::filesystem check is
    // always false for assets packaged in an APK.
    if (!asset_source().exists(resolved_path)) {
        PTV_WARN("GLB not found via asset source: {}", resolved_path);
        return -1;
    }
    const std::string key = mesh_key(resolved_path, target_scale_meters, 0.0f);
    auto it = gltf_path_to_slot_.find(key);
    if (it != gltf_path_to_slot_.end()) return it->second;

    std::vector<gltf_mesh_load::LitVertex> verts;
    std::vector<uint32_t> indices;
    std::string err;
    if (!gltf_mesh_load::load_glb_merged(resolved_path, target_scale_meters,
                                         verts, indices, err)) {
        PTV_ERROR("GLB load failed ({}): {}", resolved_path, err);
        return -1;
    }
    if (verts.empty() || indices.empty()) return -1;

    const int slot = create_mesh_slot(verts, indices);
    if (slot < 0) return -1;
    // Synchronous path (explicit load_glb). Pinned: these are not driven by
    // entity visibility, so the LRU has no signal to keep them.
    auto& gm = gltf_meshes_[static_cast<size_t>(slot)];
    gm.path = key;
    gm.evictable = false;
    gm.last_used_frame = frame_index_;
    gltf_path_to_slot_[key] = slot;
    return slot;
}

int Renderer::ensure_inline_mesh(std::uint32_t key,
                                 const std::vector<float>& positions,
                                 const std::vector<float>& normals,
                                 const std::vector<std::uint32_t>& indices,
                                 const Vec3& color) {
    if (!vk_) return -1;
    auto cached = inline_mesh_slots_.find(key);
    if (cached != inline_mesh_slots_.end()) return cached->second;

    const size_t vcount = positions.size() / 3;
    if (vcount < 3 || indices.size() < 3) return -1;

    std::vector<gltf_mesh_load::LitVertex> verts(vcount);
    const bool have_normals = normals.size() == positions.size();
    for (size_t i = 0; i < vcount; ++i) {
        verts[i].pos[0] = positions[i * 3 + 0];
        verts[i].pos[1] = positions[i * 3 + 1];
        verts[i].pos[2] = positions[i * 3 + 2];
        verts[i].normal[0] = have_normals ? normals[i * 3 + 0] : 0.0f;
        verts[i].normal[1] = have_normals ? normals[i * 3 + 1] : 0.0f;
        verts[i].normal[2] = have_normals ? normals[i * 3 + 2] : 0.0f;
        verts[i].color[0] = color.x;
        verts[i].color[1] = color.y;
        verts[i].color[2] = color.z;
        verts[i].color[3] = 1.0f;
    }

    if (!have_normals) {
        // Flat/area-weighted normals accumulated per triangle.
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            if (a >= vcount || b >= vcount || c >= vcount) continue;
            Vec3 pa{verts[a].pos[0], verts[a].pos[1], verts[a].pos[2]};
            Vec3 pb{verts[b].pos[0], verts[b].pos[1], verts[b].pos[2]};
            Vec3 pc{verts[c].pos[0], verts[c].pos[1], verts[c].pos[2]};
            Vec3 n = glm::cross(pb - pa, pc - pa);
            for (uint32_t idx : {a, b, c}) {
                verts[idx].normal[0] += n.x;
                verts[idx].normal[1] += n.y;
                verts[idx].normal[2] += n.z;
            }
        }
        for (auto& v : verts) {
            Vec3 n{v.normal[0], v.normal[1], v.normal[2]};
            float len = glm::length(n);
            if (len > 1e-6f) {
                v.normal[0] = n.x / len;
                v.normal[1] = n.y / len;
                v.normal[2] = n.z / len;
            } else {
                v.normal[1] = 1.0f;
            }
        }
    }

    const int slot = create_mesh_slot(verts, indices);
    if (slot < 0) return -1;
    inline_mesh_slots_[key] = slot;
    return slot;
}

namespace {

/** Blend a hover cue into a resting tint.
 *
 *  A multiply rather than a replace, so the cue reads as "lit up" against
 *  whatever the object already looks like instead of flattening every
 *  highlighted object to the same colour. Clamped because the shader treats
 *  colour as a tint on the source material and pushing far past 1 blows out
 *  texture detail -- exactly the detail that tells the player WHAT they are
 *  about to interact with. */
Vec4 apply_highlight(const Vec4& base, float intensity, const Vec4& cue) {
    if (intensity <= 0.0f) return base;
    const float t = std::clamp(intensity, 0.0f, 1.0f);
    Vec4 out = base;
    out.x = std::clamp(base.x * (1.0f + t * (cue.x - 1.0f) + t * 0.25f),
                       0.0f, 1.6f);
    out.y = std::clamp(base.y * (1.0f + t * (cue.y - 1.0f) + t * 0.25f),
                       0.0f, 1.6f);
    out.z = std::clamp(base.z * (1.0f + t * (cue.z - 1.0f) + t * 0.25f),
                       0.0f, 1.6f);
    return out;
}

/** Six frustum planes (x,y,z = normal, w = distance) from a view-projection.
 *  Gribb/Hartmann plane extraction; normals point INTO the frustum. */
struct Frustum {
    Vec4 planes[6];
    bool valid{false};

    void build(const Mat4& m) {
        // glm is column-major, so row K is (m[0][K], m[1][K], m[2][K], m[3][K]).
        auto row = [&m](int k) {
            return Vec4(m[0][k], m[1][k], m[2][k], m[3][k]);
        };
        const Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        planes[0] = r3 + r0;  // left
        planes[1] = r3 - r0;  // right
        planes[2] = r3 + r1;  // bottom
        planes[3] = r3 - r1;  // top
        // The engine builds projections with GLM_FORCE_DEPTH_ZERO_TO_ONE, so
        // clip-space z is in [0, w] rather than [-w, w]: the near plane is
        // plain row2, NOT row3 + row2 (the OpenGL-convention formula).
        planes[4] = r2;       // near
        planes[5] = r3 - r2;  // far
        for (auto& pl : planes) {
            const float len =
                std::sqrt(pl.x * pl.x + pl.y * pl.y + pl.z * pl.z);
            if (len > 1e-6f) pl /= len;
        }
        valid = true;
    }

    /** Conservative sphere test: false only when definitely outside. */
    bool intersects_sphere(const Vec3& c, float r) const {
        if (!valid) return true;
        for (const auto& pl : planes) {
            if (pl.x * c.x + pl.y * c.y + pl.z * c.z + pl.w < -r) return false;
        }
        return true;
    }
};

/** Largest absolute scale factor baked into a model matrix. Used to convert a
 *  mesh-space bounding radius into world space. */
float max_axis_scale(const Mat4& m) {
    const float sx = glm::length(Vec3(m[0]));
    const float sy = glm::length(Vec3(m[1]));
    const float sz = glm::length(Vec3(m[2]));
    return std::max(sx, std::max(sy, sz));
}

}  // namespace

void Renderer::build_items_from_world(World& world, std::vector<DrawItem>& out,
                                      const ViewInfo& view_info) {
    out.clear();
    stats_ = FrameStats{};
    stats_.resident_triangles = resident_triangles_;
    frame_entity_slot_.clear();
    // Palettes are rebuilt from scratch every frame alongside the draw list;
    // a stale base index from last frame would point a skinned mesh at
    // another entity's joints.
    begin_joint_palettes();

    Frustum frustum;
    if (view_info.valid) frustum.build(view_info.view_proj);
    const Vec3 cam = view_info.camera_pos;

    auto view = world.registry().view<const CTransform>();
    for (auto entity : view) {
        const auto& ct = view.get<const CTransform>(entity).t;
        ++stats_.entities_considered;

        // Draw-time pose = physics-owned CTransform + any active visual
        // offset (recoil kick, flinch). Resolved once here so every DrawItem
        // path below shares it; identical to ``ct.matrix()`` when no offset
        // is active, which is the overwhelmingly common case.
        const Mat4 entity_model = entity_visual_matrix(
            world, static_cast<std::uint32_t>(entity), ct);

        // --- Skip invisible / logic-only entities --------------------------------
        // Generated scenes often include dozens of ``mesh: null`` trigger /
        // tutorial volumes. Without this guard the renderer falls through to
        // the procedural cube path, littering the world with cyan "starter
        // room" debug cubes *on top of* the real Tripo environment. Template
        // meshes (construct prefabs parked at a far staging coordinate) are
        // also hidden so only runtime-spawned copies draw.
        //
        // Resolved ONCE here, from the table in VisualTagTable.h, and reused
        // by the mesh, primitive and placeholder paths below. They each used
        // to re-test their own string literals, which is how a declared door
        // ended up drawing as an orange grab-cube: the door row existed in one
        // path and not the others.
        render_tags::TagVisuals visuals{};
        if (world.registry().all_of<CTag>(entity)) {
            visuals = render_tags::resolve(
                world.registry().get<CTag>(entity).tags);
            if (visuals.always_hidden) {
                continue;
            }
            const bool meshless =
                !world.registry().all_of<CMesh>(entity) ||
                world.registry().get<CMesh>(entity).glb_path.empty();
            if (meshless && visuals.hidden_when_meshless) {
                continue;
            }
        }

        // The hover cue, published by whichever affordance built-in owns this
        // object. The renderer never decides whether something is actionable;
        // it only draws the answer.
        float highlight = 0.0f;
        Vec4 highlight_color{1.0f, 0.92f, 0.62f, 1.0f};
        if (world.registry().all_of<CHighlight>(entity)) {
            const auto& hl = world.registry().get<CHighlight>(entity);
            highlight = std::clamp(hl.intensity, 0.0f, 1.0f);
            highlight_color = hl.color;
        }

        if (world.registry().all_of<CMesh>(entity)) {
            const auto& cm = world.registry().get<CMesh>(entity);
            if (!cm.glb_path.empty()) {
                const Mat4 model = entity_model;
                const Vec3 center{model[3][0], model[3][1], model[3][2]};
                const float distance = view_info.valid
                                           ? glm::length(center - cam)
                                           : 0.0f;

                // Radius: prefer the pipeline-computed value (already scaled
                // to metres); otherwise fall back to a generous estimate so a
                // mesh is never culled just because we lack bounds.
                float radius = cm.bounds_radius_m;
                if (radius <= 0.0f) {
                    radius = std::max(1.0f, cm.target_scale_meters) *
                             max_axis_scale(model);
                }

                // The player's own body is drawn every frame at full detail:
                // the camera sits inside it, so a bounds test against a
                // posed, IK-driven body is a guess, and any pop is right in
                // the player's face.
                bool first_person = world.registry().all_of<CSelfViewHide>(entity);
                if (!first_person) {
                    if (const auto* tag = world.registry().try_get<CTag>(entity)) {
                        first_person =
                            std::find(tag->tags.begin(), tag->tags.end(),
                                      "player_avatar") != tag->tags.end();
                    }
                }

                // Distance cull. cull_distance_m_ is effectively infinite for
                // compact/standard scenes, so small games still show
                // everything; the governor may tighten it under load.
                if (!first_person && view_info.valid &&
                    distance - radius > cull_distance_m_) {
                    ++stats_.culled_distance;
                    continue;
                }
                // Frustum cull once for both eyes.
                if (!first_person && view_info.valid &&
                    !frustum.intersects_sphere(center, radius)) {
                    ++stats_.culled_frustum;
                    continue;
                }

                // --- Pick a level of detail -------------------------------
                // Choose by distance against the scene's switch distances,
                // shrunk by the governor's lod_bias when frames are slow.
                const auto& chain = cm.lod_paths;
                int desired = 0;
                if (chain.size() > 1 && view_info.valid && !first_person) {
                    const float bias = 1.0f / (1.0f + std::max(0.0f, lod_bias_));
                    const auto& sw = perf_.lod_switch_distances_m;
                    for (size_t i = 0; i < sw.size() &&
                                       i < chain.size() - 1;
                         ++i) {
                        if (distance > sw[i] * bias) {
                            desired = static_cast<int>(i) + 1;
                        }
                    }
                    // Hysteresis: only move one level per frame and require a
                    // clear margin, so an object hovering on a switch boundary
                    // does not visibly pop back and forth.
                    const auto prev_it =
                        entity_lod_.find(static_cast<std::uint32_t>(entity));
                    if (prev_it != entity_lod_.end()) {
                        const int prev = prev_it->second;
                        if (desired > prev) {
                            desired = prev + 1;
                        } else if (desired < prev) {
                            const size_t idx = static_cast<size_t>(desired);
                            const bool clearly_closer =
                                idx < sw.size() &&
                                distance < sw[idx] * bias * 0.85f;
                            desired = clearly_closer ? prev - 1 : prev;
                        }
                    }
                    desired = std::max(
                        0, std::min(desired,
                                    static_cast<int>(chain.size()) - 1));
                }

                // --- Resolve to a resident mesh ---------------------------
                // Never block on I/O here. Request the LOD we want and, until
                // it arrives, draw any coarser level that is already resident.
                const std::string& wanted =
                    chain.empty() ? cm.glb_path
                                  : chain[static_cast<size_t>(desired)];
                // A rigged entity must never go through the streaming worker:
                // that path parses GLBs as 40-byte static geometry and would
                // cache a static slot under the skinned mesh's own path, after
                // which the entity draws with the wrong pipeline forever. The
                // scene loader uploads skinned meshes up front instead.
                const CSkeleton* skel =
                    world.registry().try_get<CSkeleton>(entity);
                // Skinned meshes are uploaded by target scale at scene load;
                // static ones stream with the asset's single unit scale when
                // the pipeline provided one.
                const float unit = skel ? 0.0f : cm.unit_scale;
                auto key_of = [&](const std::string& p) {
                    return mesh_key(p, cm.target_scale_meters, unit);
                };
                int slot = resident_slot(key_of(wanted));
                if (slot < 0 && !skel) {
                    request_stream(wanted, cm.target_scale_meters, unit,
                                   distance);
                    for (size_t i = static_cast<size_t>(desired) + 1;
                         slot < 0 && i < chain.size(); ++i) {
                        slot = resident_slot(key_of(chain[i]));
                    }
                    if (slot < 0 && !chain.empty()) {
                        slot = resident_slot(key_of(chain[0]));
                    }
                }
                if (slot >= 0) {
                    entity_lod_[static_cast<std::uint32_t>(entity)] = desired;
                    // Remember which slot this entity resolved to, so the
                    // mirror pass can reuse it instead of re-deriving the LOD
                    // path (and so it silently skips anything not yet
                    // streamed in).
                    frame_entity_slot_[static_cast<std::uint32_t>(entity)] =
                        slot;
                    DrawItem it{};
                    it.model = model;
                    it.mesh_slot = slot;
                    it.material_id = 0.0f;
                    // Hand this frame's pose to the shared palette buffer.
                    // Every LOD of a rigged asset carries the same joint list
                    // by construction (see ptv_glb_optimize), so one palette
                    // serves whichever level got picked above.
                    if (skel && !skel->palette.empty() &&
                        gltf_meshes_[static_cast<size_t>(slot)].skinned) {
                        // This list is the PLAYER'S OWN view, so an avatar
                        // carrying a self-view suppression gets the collapsed
                        // palette here. The full one stays untouched for any
                        // other view of the same entity (a mirror), which is
                        // why there are two rather than one edited in place.
                        const auto& palette =
                            skel->palette_self_view.size() ==
                                    skel->palette.size()
                                ? skel->palette_self_view
                                : skel->palette;
                        it.joint_base = push_joint_palette(
                            palette.data(),
                            static_cast<int>(palette.size()));
                        it.joint_count = static_cast<int>(palette.size());
                    }
                    // GLB primitives carry their own per-vertex baseColor
                    // (see GltfMeshLoad). Use a near-white tint so the
                    // material color drives the look; we only nudge the
                    // tint for category cues and never crush the source.
                    it.color = visuals.has_mesh_tint
                                   ? visuals.mesh_tint
                                   : Vec4{1.0f, 1.0f, 1.0f, 1.0f};
                    it.color = apply_highlight(it.color, highlight,
                                               highlight_color);
                    out.push_back(it);
                    ++stats_.drawn;
                    stats_.visible_triangles +=
                        gltf_meshes_[static_cast<size_t>(slot)].triangle_count;
                    continue;
                }
                // Nothing resident yet: skip this entity for now. It pops in
                // within a frame or two as the streaming worker catches up --
                // vastly preferable to a multi-second stall on frame one.
                continue;
            }
        }

        // --- Procedural primitive from the world-generation stage ----------
        if (world.registry().all_of<CPrimitive>(entity)) {
            const auto& prim = world.registry().get<CPrimitive>(entity);
            DrawItem it{};
            Mat4 s = glm::scale(Mat4(1.0f),
                                Vec3(prim.size.x, prim.size.y, prim.size.z));
            it.model = entity_model * s;
            it.color = apply_highlight(
                Vec4{prim.color.x, prim.color.y, prim.color.z, 1.0f},
                highlight, highlight_color);
            it.material_id = visuals.is_ground ? 1.0f : 0.0f;
            // Select the matching unit mesh by kind (all authored at unit
            // extent so the scale-by-size path above is identical to the box).
            // "box"/unknown falls back to the cube (mesh_slot = -1). These
            // mirror the collision shapes built in PhysicsWorld.
            if (prim.kind == "sphere") {
                it.mesh_slot = prim_slot_sphere_;
            } else if (prim.kind == "cylinder") {
                it.mesh_slot = prim_slot_cylinder_;
            } else if (prim.kind == "plane") {
                it.mesh_slot = prim_slot_plane_;
            } else if (prim.kind == "ramp") {
                it.mesh_slot = prim_slot_ramp_;
            } else {
                it.mesh_slot = -1;  // box / unknown -> unit cube
            }
            // World-gen primitives are cheap and always resident, but they
            // still count toward the frame's visible triangle total that the
            // governor watches.
            if (it.mesh_slot >= 0) {
                stats_.visible_triangles +=
                    gltf_meshes_[static_cast<size_t>(it.mesh_slot)]
                        .triangle_count;
            } else {
                stats_.visible_triangles += cube_index_count_ / 3;
            }
            out.push_back(it);
            ++stats_.drawn;
            continue;
        }

        // --- Arbitrary procedural mesh from the world-generation stage ------
        if (world.registry().all_of<CInlineMesh>(entity)) {
            const auto& im = world.registry().get<CInlineMesh>(entity);
            int slot = ensure_inline_mesh(static_cast<std::uint32_t>(entity),
                                          im.positions, im.normals, im.indices,
                                          im.color);
            if (slot >= 0) {
                DrawItem it{};
                it.model = entity_model;
                it.mesh_slot = slot;
                it.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};  // vertex colors drive look
                it.material_id = 0.0f;
                out.push_back(it);
                ++stats_.drawn;
                stats_.visible_triangles +=
                    gltf_meshes_[static_cast<size_t>(slot)].triangle_count;
                continue;
            }
        }

        DrawItem it;
        it.model = entity_model;
        it.mesh_slot = -1;

        // Size + colour come from the same table as everything else, so a
        // newly declared affordance gets its own placeholder appearance
        // without a renderer edit. A floor is oversized here so its edge never
        // shows in the headset; the fragment shader fades a grid inside the
        // render-distance horizon.
        // Bake a non-uniform scale into the model matrix without touching
        // the Transform's stored scale (which is uniform).
        Mat4 s = glm::scale(
            Mat4(1.0f), Vec3(visuals.placeholder_xz_scale,
                             visuals.placeholder_y_scale,
                             visuals.placeholder_xz_scale));
        it.model = it.model * s;
        it.color = apply_highlight(visuals.placeholder_color, highlight,
                                   highlight_color);
        it.material_id = visuals.placeholder_material_id;
        out.push_back(it);
        ++stats_.drawn;
        stats_.visible_triangles += cube_index_count_ / 3;
    }

    append_mirror_reflections(world, out, view_info);

    stats_.resident_meshes =
        static_cast<std::uint32_t>(gltf_path_to_slot_.size());
    stats_.resident_triangles = resident_triangles_;
}

void Renderer::append_mirror_reflections(World& world,
                                         std::vector<DrawItem>& out,
                                         const ViewInfo& view) {
    stats_.mirror_reflections = 0;
    auto mirrors = world.registry().view<const CMirrorSurface,
                                         const CTransform>();

    // One mirror at a time. Reflecting several would multiply the reflected
    // draw calls, which is the classic mobile-VR trap this design exists to
    // avoid.
    const CMirrorSurface* chosen = nullptr;
    Vec3 plane_point{0.0f};
    float best = std::numeric_limits<float>::max();
    for (auto e : mirrors) {
        const auto& m = mirrors.get<const CMirrorSurface>(e);
        if (!m.active) continue;
        const Vec3 p = mirrors.get<const CTransform>(e).t.position;
        const float d = view.valid ? glm::length(view.camera_pos - p) : 0.0f;
        if (d > m.max_distance_m || d >= best) continue;
        best = d;
        chosen = &m;
        plane_point = p;
    }
    if (chosen == nullptr) return;

    const float nl = glm::length(chosen->normal);
    if (nl < 1.0e-4f) return;
    const Vec3 n = chosen->normal / nl;

    // Householder reflection about the plane through plane_point with normal
    // n. Column-major, so the translation term lands in the last column.
    const float d_plane = -glm::dot(n, plane_point);
    Mat4 reflect(1.0f);
    reflect[0][0] = 1.0f - 2.0f * n.x * n.x;
    reflect[1][0] = -2.0f * n.x * n.y;
    reflect[2][0] = -2.0f * n.x * n.z;
    reflect[3][0] = -2.0f * n.x * d_plane;
    reflect[0][1] = -2.0f * n.y * n.x;
    reflect[1][1] = 1.0f - 2.0f * n.y * n.y;
    reflect[2][1] = -2.0f * n.y * n.z;
    reflect[3][1] = -2.0f * n.y * d_plane;
    reflect[0][2] = -2.0f * n.z * n.x;
    reflect[1][2] = -2.0f * n.z * n.y;
    reflect[2][2] = 1.0f - 2.0f * n.z * n.z;
    reflect[3][2] = -2.0f * n.z * d_plane;

    auto reflected_wanted = [&](entt::entity e) {
        if (!world.registry().all_of<CTag>(e)) return false;
        for (const auto& t : world.registry().get<CTag>(e).tags) {
            if (t == "player_avatar" || t == "mirror_visible") return true;
            for (const auto& want : chosen->reflect_tags) {
                if (t == want) return true;
            }
        }
        return false;
    };

    int emitted = 0;
    auto candidates = world.registry().view<const CTransform, const CMesh>();
    for (auto e : candidates) {
        if (emitted >= chosen->max_reflected) break;
        if (!reflected_wanted(e)) continue;

        const auto slot_it =
            frame_entity_slot_.find(static_cast<std::uint32_t>(e));
        if (slot_it == frame_entity_slot_.end()) continue;
        const int slot = slot_it->second;
        if (slot < 0 || static_cast<size_t>(slot) >= gltf_meshes_.size()) {
            continue;
        }
        auto& gm = gltf_meshes_[static_cast<size_t>(slot)];
        if (gm.vb == VK_NULL_HANDLE) continue;
        gm.last_used_frame = frame_index_;

        DrawItem it{};
        it.model = reflect * entity_visual_matrix(
                                 world, static_cast<std::uint32_t>(e),
                                 world.registry().get<CTransform>(e).t);
        it.mesh_slot = slot;
        it.mirrored = true;
        it.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};

        // The FULL palette, deliberately. The self-view copy above may have
        // the head collapsed so the camera is not inside the skull; the
        // reflection has to show a complete avatar, which is the entire
        // reason two palettes exist.
        if (gm.skinned && world.registry().all_of<CSkeleton>(e)) {
            const auto& sk = world.registry().get<CSkeleton>(e);
            if (!sk.palette.empty()) {
                it.joint_base = push_joint_palette(
                    sk.palette.data(), static_cast<int>(sk.palette.size()));
                it.joint_count = static_cast<int>(sk.palette.size());
            } else {
                continue;
            }
        } else if (gm.skinned) {
            continue;
        }

        out.push_back(it);
        ++emitted;
        ++stats_.drawn;
        stats_.visible_triangles += gm.triangle_count;
    }
    stats_.mirror_reflections = static_cast<std::uint32_t>(emitted);
}

void Renderer::shutdown() {
    // Join the streaming worker BEFORE touching Vulkan objects: it must not be
    // mid-parse when the device goes away. Safe to call when never started.
    stop_stream_worker();
    if (!vk_) return;
    if (vk_->device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vk_->device());
    }
    destroy_gltf_meshes();
    for (auto& kv : framebuffers_) {
        if (kv.second) vkDestroyFramebuffer(vk_->device(), kv.second, nullptr);
    }
    framebuffers_.clear();
    for (auto& kv : color_views_) {
        if (kv.second) vkDestroyImageView(vk_->device(), kv.second, nullptr);
    }
    color_views_.clear();
    if (depth_view_) vkDestroyImageView(vk_->device(), depth_view_, nullptr);
    if (depth_image_) vmaDestroyImage(vk_->allocator(), depth_image_, depth_alloc_);
    depth_view_ = VK_NULL_HANDLE;
    depth_image_ = VK_NULL_HANDLE;
    depth_alloc_ = nullptr;

    if (msaa_color_view_)
        vkDestroyImageView(vk_->device(), msaa_color_view_, nullptr);
    if (msaa_color_image_)
        vmaDestroyImage(vk_->allocator(), msaa_color_image_, msaa_color_alloc_);
    msaa_color_view_ = VK_NULL_HANDLE;
    msaa_color_image_ = VK_NULL_HANDLE;
    msaa_color_alloc_ = nullptr;

    if (cube_vb_) vmaDestroyBuffer(vk_->allocator(), cube_vb_, cube_vb_alloc_);
    if (cube_ib_) vmaDestroyBuffer(vk_->allocator(), cube_ib_, cube_ib_alloc_);
    cube_vb_ = cube_ib_ = VK_NULL_HANDLE;
    cube_vb_alloc_ = cube_ib_alloc_ = nullptr;

    // Environment UBO + descriptors (VMA unmaps the persistently-mapped buffer
    // on destroy).
    if (env_ubo_) vmaDestroyBuffer(vk_->allocator(), env_ubo_, env_ubo_alloc_);
    env_ubo_ = VK_NULL_HANDLE;
    env_ubo_alloc_ = nullptr;
    env_ubo_mapped_ = nullptr;
    if (joint_ssbo_)
        vmaDestroyBuffer(vk_->allocator(), joint_ssbo_, joint_ssbo_alloc_);
    joint_ssbo_ = VK_NULL_HANDLE;
    joint_ssbo_alloc_ = nullptr;
    joint_ssbo_mapped_ = nullptr;
    joint_ssbo_capacity_ = 0;
    joint_palettes_.clear();
    if (desc_pool_) vkDestroyDescriptorPool(vk_->device(), desc_pool_, nullptr);
    if (env_set_layout_)
        vkDestroyDescriptorSetLayout(vk_->device(), env_set_layout_, nullptr);
    desc_pool_ = VK_NULL_HANDLE;
    env_set_layout_ = VK_NULL_HANDLE;
    env_set_ = VK_NULL_HANDLE;

    if (vignette_pipeline_)
        vkDestroyPipeline(vk_->device(), vignette_pipeline_, nullptr);
    if (vignette_pipeline_layout_)
        vkDestroyPipelineLayout(vk_->device(), vignette_pipeline_layout_,
                                nullptr);
    if (vignette_fs_) vkDestroyShaderModule(vk_->device(), vignette_fs_, nullptr);
    vignette_pipeline_ = VK_NULL_HANDLE;
    vignette_pipeline_layout_ = VK_NULL_HANDLE;
    vignette_fs_ = VK_NULL_HANDLE;
    if (sky_pipeline_) vkDestroyPipeline(vk_->device(), sky_pipeline_, nullptr);
    if (sky_pipeline_layout_)
        vkDestroyPipelineLayout(vk_->device(), sky_pipeline_layout_, nullptr);
    if (sky_vs_) vkDestroyShaderModule(vk_->device(), sky_vs_, nullptr);
    if (sky_fs_) vkDestroyShaderModule(vk_->device(), sky_fs_, nullptr);
    sky_pipeline_ = VK_NULL_HANDLE;
    sky_pipeline_layout_ = VK_NULL_HANDLE;
    sky_vs_ = sky_fs_ = VK_NULL_HANDLE;

    if (pipeline_) vkDestroyPipeline(vk_->device(), pipeline_, nullptr);
    if (skinned_pipeline_)
        vkDestroyPipeline(vk_->device(), skinned_pipeline_, nullptr);
    if (pipeline_layout_)
        vkDestroyPipelineLayout(vk_->device(), pipeline_layout_, nullptr);
    if (vs_) vkDestroyShaderModule(vk_->device(), vs_, nullptr);
    if (fs_) vkDestroyShaderModule(vk_->device(), fs_, nullptr);
    if (skinned_vs_) vkDestroyShaderModule(vk_->device(), skinned_vs_, nullptr);
    if (render_pass_) vkDestroyRenderPass(vk_->device(), render_pass_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    skinned_pipeline_ = VK_NULL_HANDLE;
    if (pipeline_mirror_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_->device(), pipeline_mirror_, nullptr);
        pipeline_mirror_ = VK_NULL_HANDLE;
    }
    if (skinned_pipeline_mirror_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(vk_->device(), skinned_pipeline_mirror_, nullptr);
        skinned_pipeline_mirror_ = VK_NULL_HANDLE;
    }
    pipeline_layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = skinned_vs_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;

    if (frame_fence_) vkDestroyFence(vk_->device(), frame_fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(vk_->device(), cmd_pool_, nullptr);
    frame_fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

}  // namespace ptv
