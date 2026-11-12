#include "ptv/render/Renderer.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "ptv/core/Logger.h"
#include "ptv/core/MemoryGuard.h"
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

// Push constants shared by every pipeline on pipeline_layout_ (scene, sky,
// vignette). ``view_base`` selects the eye when the frame is drawn one view
// per pass; with multiview it is 0 and gl_ViewIndex picks the eye.
struct PushConstants {
    std::uint32_t view_base;
    std::uint32_t pad[3];
    Vec4 p0;  // vignette: clear half-angle rad, feather rad, opacity, unused
    Vec4 p1;  // vignette: color
};
static_assert(sizeof(PushConstants) == 48, "push constant block layout");

constexpr int kMaxPointLights = 8;

// Frame uniform block (set 0, binding 0). Layout MUST match ``FrameUBO`` in
// scene.vert / scene.frag / sky.frag / vignette.frag (std140; every member is
// a vec4 or mat4 so there is no padding ambiguity).
struct GpuPointLight {
    float pos_range[4];        // xyz = world position, w = range (m)
    float color_intensity[4];  // rgb = color, w = intensity
};
struct GpuFrame {
    Mat4 view_proj[2];
    Mat4 inv_view_proj[2];
    Mat4 inv_proj[2];
    float camera_pos[2][4];  // xyz = eye world position
    float sun_dir[4];        // xyz = travel direction (normalized), w unused
    float sun_color[4];      // rgb = color, w = intensity
    float sky_top[4];        // rgb = zenith color, w = exposure
    float sky_horizon[4];    // rgb = horizon/fog color, w = fog density
    float ambient[4];        // rgb = ground bounce color, w = ambient intensity
    float misc[4];           // x = active point lights
    GpuPointLight points[kMaxPointLights];
};
static_assert(sizeof(GpuFrame) == 768, "FrameUBO layout");

// One per draw (set 0, binding 2), indexed by gl_InstanceIndex. Instanced
// draws read consecutive entries.
struct GpuInstance {
    Mat4 model;
    Vec4 color;
    Vec4 uv_xform;  // xy = uv offset, zw = uv scale (dequantisation)
    Vec4 params;    // x = material id, y = joint base, z = alpha cutoff
};
static_assert(sizeof(GpuInstance) == 112, "Instance layout");

// GPU vertex layouts. Positions stay full float; normals are octahedral
// 16-bit, UVs 16-bit normalised over the mesh's own UV range, colour 8-bit:
// half the bytes of the float layout, which matters on a bandwidth-bound
// mobile GPU (Meta: "Compress your mesh vertices").
struct GpuVertex {
    float pos[3];
    std::int16_t normal[2];   // R16G16_SNORM, octahedral
    std::uint16_t uv[2];      // R16G16_UNORM
    std::uint8_t color[4];    // R8G8B8A8_UNORM
};
static_assert(sizeof(GpuVertex) == 24, "GpuVertex layout");
struct GpuSkinnedVertex {
    GpuVertex v;
    std::uint8_t joints[4];   // R8G8B8A8_UINT
    std::uint8_t weights[4];  // R8G8B8A8_UNORM
};
static_assert(sizeof(GpuSkinnedVertex) == 32, "GpuSkinnedVertex layout");

std::int16_t snorm16(float v) {
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

void encode_oct(const float n_in[3], std::int16_t out[2]) {
    float n[3] = {n_in[0], n_in[1], n_in[2]};
    const float len = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
    if (len < 1e-8f) {
        out[0] = 0;
        out[1] = snorm16(1.0f);  // +Y-ish fallback after decode normalisation
        return;
    }
    float x = n[0] / len, y = n[1] / len;
    if (n[2] < 0.0f) {
        const float ox = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    out[0] = snorm16(x);
    out[1] = snorm16(y);
}

std::uint8_t unorm8(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

/** UV range of a vertex set, for 16-bit dequantisation. */
template <class V>
Vec4 uv_range(const std::vector<V>& verts) {
    float lo[2] = {FLT_MAX, FLT_MAX}, hi[2] = {-FLT_MAX, -FLT_MAX};
    for (const auto& v : verts) {
        for (int k = 0; k < 2; ++k) {
            lo[k] = std::min(lo[k], v.uv[k]);
            hi[k] = std::max(hi[k], v.uv[k]);
        }
    }
    if (verts.empty()) return Vec4(0.0f, 0.0f, 1.0f, 1.0f);
    const float sx = hi[0] - lo[0] > 1e-8f ? hi[0] - lo[0] : 1.0f;
    const float sy = hi[1] - lo[1] > 1e-8f ? hi[1] - lo[1] : 1.0f;
    return Vec4(lo[0], lo[1], sx, sy);
}

template <class V>
GpuVertex pack_vertex(const V& v, const Vec4& uvx) {
    GpuVertex g{};
    std::memcpy(g.pos, v.pos, sizeof(g.pos));
    encode_oct(v.normal, g.normal);
    for (int k = 0; k < 2; ++k) {
        const float t = (v.uv[k] - uvx[k]) / uvx[2 + k];
        g.uv[k] = static_cast<std::uint16_t>(std::lround(std::clamp(t, 0.0f, 1.0f) * 65535.0f));
    }
    for (int k = 0; k < 4; ++k) g.color[k] = unorm8(v.color[k]);
    return g;
}

// ---------------------------------------------------------------------------
// Built-in unit primitive meshes for the world-generation ``spawn_primitive``
// kinds. Each is authored at unit extent (fits the [-0.5, 0.5]^3 box) so
// ``build_items_from_world`` can scale by the primitive's ``size``. Vertices
// ship white so the per-DrawItem tint (the primitive's color) drives the
// final look.
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

// Unit cube centred at the origin, one flat normal per face.
void make_cube(std::vector<LitVertex>& v, std::vector<uint32_t>& idx) {
    const float h = 0.5f;
    push_quad(v, idx, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}, {1, 0, 0});
    push_quad(v, idx, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}, {-1, 0, 0});
    push_quad(v, idx, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}, {0, 1, 0});
    push_quad(v, idx, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {0, -1, 0});
    push_quad(v, idx, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1});
    push_quad(v, idx, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1});
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

std::uint64_t image_key(VkImage img, std::uint32_t layer) {
    return (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(img)) << 2) ^ layer;
}

}  // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::initialize(VulkanContext& vk, VkFormat color_format,
                          uint32_t eye_width, uint32_t eye_height) {
    TargetInfo t;
    t.color_format = color_format;
    t.width = eye_width;
    t.height = eye_height;
    return initialize(vk, t);
}

bool Renderer::initialize(VulkanContext& vk, const TargetInfo& target) {
    vk_ = &vk;
    color_format_ = target.color_format;
    eye_width_ = target.width;
    eye_height_ = target.height;
    multiview_ = target.multiview && vk.multiview_enabled();
    fdm_ = target.fragment_density_map && vk.fragment_density_map_enabled();
    samples_ = pick_sample_count();
    if (!textures_.initialize(vk, vk.anisotropy_enabled())) return false;
    if (!create_render_pass()) return false;
    if (!create_descriptors()) return false;
    if (!create_pipeline_layout()) return false;
    // The everyday variants up front; alpha-test / double-sided ones are
    // built the first time a mesh needs them (at upload, not mid-draw).
    const std::uint32_t common_variants[4] = {
        0u, kPipeSkinned, kPipeMirrored,
        static_cast<std::uint32_t>(kPipeSkinned) | static_cast<std::uint32_t>(kPipeMirrored)};
    for (std::uint32_t bits : common_variants) {
        if (scene_pipeline(bits) == VK_NULL_HANDLE) return false;
    }
    if (!create_sky_pipeline()) return false;
    // Optional: a missing vignette shader costs the comfort option, not the
    // game.
    if (!create_vignette_pipeline()) {
        PTV_WARN("Comfort vignette pipeline unavailable; vignettes disabled");
    }
    if (!create_primitive_meshes()) return false;
    if (!create_depth_image()) return false;
    if (!create_msaa_color_image()) return false;
    if (!create_command_resources()) return false;
    // Optional like the vignette: a missing font or shader costs the labels,
    // not the game.
    text_.initialize(vk, render_pass_, samples_, cmd_pool_);
    fx_.initialize(vk, render_pass_, samples_);
    start_stream_worker();
    PTV_INFO("Renderer ready: {}x{} per eye, color fmt {}, MSAA {}x, {}{}",
             eye_width_, eye_height_, static_cast<int>(color_format_),
             static_cast<int>(samples_),
             multiview_ ? "multiview (one pass for both eyes)" : "one pass per eye",
             fdm_ ? ", fragment density map (FFR)" : "");
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

    // Last attachment (FFR only): the runtime's fragment density map for this
    // swapchain image. Read-only; the runtime owns its contents.
    VkAttachmentDescription density{};
    density.format = VK_FORMAT_R8G8_UNORM;
    density.samples = VK_SAMPLE_COUNT_1_BIT;
    density.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    density.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    density.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    density.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    density.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    density.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;

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

    std::array<VkAttachmentDescription, 4> atts{color, depth, resolve, density};
    uint32_t count = msaa ? 3u : 2u;
    if (fdm_) {
        atts[count] = density;
        ++count;
    }
    // With two frames in flight consecutive passes share the transient depth
    // and MSAA targets: order this pass's clears after the previous pass's
    // attachment writes.
    constexpr VkPipelineStageFlags kAttachStages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = kAttachStages;
    deps[0].dstStageMask = kAttachStages;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = 0;

    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = count;
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = static_cast<uint32_t>(deps.size());
    ci.pDependencies = deps.data();

    // Both eyes in one pass: every draw is broadcast to layers 0 and 1, and
    // the two views are correlated (similar), which lets the driver share
    // work between them.
    const uint32_t view_mask = 0b11;
    VkRenderPassMultiviewCreateInfo mv{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
    mv.subpassCount = 1;
    mv.pViewMasks = &view_mask;
    mv.correlationMaskCount = 1;
    mv.pCorrelationMasks = &view_mask;
    VkRenderPassFragmentDensityMapCreateInfoEXT fdm{
        VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
    fdm.fragmentDensityMapAttachment = {count - 1,
                                        VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
    const void* chain = nullptr;
    if (fdm_) {
        fdm.pNext = chain;
        chain = &fdm;
    }
    if (multiview_) {
        mv.pNext = chain;
        chain = &mv;
    }
    ci.pNext = chain;
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
    if (frame_set_ != VK_NULL_HANDLE) {
        VkDescriptorBufferInfo dbi{joint_ssbo_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = frame_set_;
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

bool Renderer::ensure_instance_buffer(std::uint32_t instance_capacity) {
    if (instance_capacity <= instance_capacity_ && instance_ssbo_) return true;
    std::uint32_t cap = std::max<std::uint32_t>(instance_capacity_, 512u);
    while (cap < instance_capacity) cap *= 2u;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = static_cast<VkDeviceSize>(cap) * sizeof(GpuInstance);
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ainfo{};
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &buf, &alloc, &ainfo) != VK_SUCCESS) {
        PTV_ERROR("failed to allocate instance buffer ({} instances)", cap);
        return false;
    }
    if (instance_ssbo_) vmaDestroyBuffer(vk_->allocator(), instance_ssbo_, instance_ssbo_alloc_);
    instance_ssbo_ = buf;
    instance_ssbo_alloc_ = alloc;
    instance_ssbo_mapped_ = ainfo.pMappedData;
    instance_capacity_ = cap;
    if (frame_set_ != VK_NULL_HANDLE) {
        VkDescriptorBufferInfo dbi{instance_ssbo_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = frame_set_;
        w.dstBinding = 2;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(vk_->device(), 1, &w, 0, nullptr);
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

void Renderer::select_frame_slot(int slot) {
    slot = std::clamp(slot, 0, kMaxFramesInFlight - 1);
    if (slot == frame_slot_) return;
    FrameSlot& cur = frame_slots_[static_cast<size_t>(frame_slot_)];
    cur.cmd = cmd_;
    cur.fence = frame_fence_;
    cur.set = frame_set_;
    cur.ubo = frame_ubo_;
    cur.ubo_alloc = frame_ubo_alloc_;
    cur.ubo_mapped = frame_ubo_mapped_;
    cur.joints = joint_ssbo_;
    cur.joints_alloc = joint_ssbo_alloc_;
    cur.joints_mapped = joint_ssbo_mapped_;
    cur.joints_capacity = joint_ssbo_capacity_;
    cur.instances = instance_ssbo_;
    cur.instances_alloc = instance_ssbo_alloc_;
    cur.instances_mapped = instance_ssbo_mapped_;
    cur.instances_capacity = instance_capacity_;
    const FrameSlot& nx = frame_slots_[static_cast<size_t>(slot)];
    cmd_ = nx.cmd;
    frame_fence_ = nx.fence;
    frame_set_ = nx.set;
    frame_ubo_ = nx.ubo;
    frame_ubo_alloc_ = nx.ubo_alloc;
    frame_ubo_mapped_ = nx.ubo_mapped;
    joint_ssbo_ = nx.joints;
    joint_ssbo_alloc_ = nx.joints_alloc;
    joint_ssbo_mapped_ = nx.joints_mapped;
    joint_ssbo_capacity_ = nx.joints_capacity;
    instance_ssbo_ = nx.instances;
    instance_ssbo_alloc_ = nx.instances_alloc;
    instance_ssbo_mapped_ = nx.instances_mapped;
    instance_capacity_ = nx.instances_capacity;
    frame_slot_ = slot;
}

void Renderer::set_frames_in_flight(int n) {
#if defined(__ANDROID__)
    frames_in_flight_ = std::clamp(n, 1, kMaxFramesInFlight);
#else
    (void)n;
    frames_in_flight_ = 1;
#endif
}

bool Renderer::create_descriptors() {
    // Set 0, binding 0: frame UBO (both eyes' matrices + environment).
    // Set 0, binding 1: joint matrix palettes (vertex stage).
    // Set 0, binding 2: per-draw instances (vertex + fragment).
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(vk_->device(), &lci, nullptr,
                                    &frame_set_layout_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize ps[2]{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kMaxFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = kMaxFramesInFlight;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(vk_->device(), &pci, nullptr, &desc_pool_) !=
        VK_SUCCESS) {
        return false;
    }

    // One UBO + set + joint and instance buffers per frame in flight.
    for (int s = kMaxFramesInFlight - 1; s >= 0; --s) {
        select_frame_slot(s);
        // Persistently-mapped host-visible UBO, refreshed once per frame.
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizeof(GpuFrame);
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ainfo{};
        if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &frame_ubo_, &frame_ubo_alloc_,
                            &ainfo) != VK_SUCCESS) {
            return false;
        }
        frame_ubo_mapped_ = ainfo.pMappedData;

        VkDescriptorSetAllocateInfo dsi{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsi.descriptorPool = desc_pool_;
        dsi.descriptorSetCount = 1;
        dsi.pSetLayouts = &frame_set_layout_;
        if (vkAllocateDescriptorSets(vk_->device(), &dsi, &frame_set_) != VK_SUCCESS) {
            return false;
        }

        // Bindings 1 and 2 must reference real buffers even in a scene with
        // no skinned meshes at all: the layout declares them.
        if (!ensure_joint_buffer(256u)) return false;
        if (!ensure_instance_buffer(512u)) return false;

        VkDescriptorBufferInfo dbi{frame_ubo_, 0, sizeof(GpuFrame)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = frame_set_;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(vk_->device(), 1, &w, 0, nullptr);
    }
    return true;
}

bool Renderer::create_pipeline_layout() {
    const std::string base = std::string(PTV_SHADER_DIR);
    vs_ = make_shader(vk_->device(), base + "/scene.vert.spv");
    fs_ = make_shader(vk_->device(), base + "/scene.frag.spv");
    skinned_vs_ = make_shader(vk_->device(), base + "/scene_skinned.vert.spv");
    if (!vs_ || !fs_ || !skinned_vs_) return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(PushConstants);
    const VkDescriptorSetLayout sets[2] = {frame_set_layout_, textures_.set_layout()};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = sets;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    return vkCreatePipelineLayout(vk_->device(), &plci, nullptr, &pipeline_layout_) ==
           VK_SUCCESS;
}

VkPipeline Renderer::scene_pipeline(std::uint32_t bits) {
    bits &= kPipeVariants - 1;
    if (pipelines_[bits] != VK_NULL_HANDLE) return pipelines_[bits];
    const bool skinned = (bits & kPipeSkinned) != 0;
    const bool mirrored = (bits & kPipeMirrored) != 0;
    const bool alpha_test = (bits & kPipeAlphaTest) != 0;
    const bool cull_none = (bits & kPipeCullNone) != 0;

    // Alpha test is a specialisation constant, not a runtime branch: a
    // fragment shader that CAN discard forces late depth testing on tiled
    // GPUs, so opaque draws must use a variant where the discard is compiled
    // out entirely.
    const VkBool32 alpha_value = alpha_test ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry spec_entry{0, 0, sizeof(VkBool32)};
    VkSpecializationInfo spec{1, &spec_entry, sizeof(VkBool32), &alpha_value};

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = skinned ? skinned_vs_ : vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &spec;

    VkVertexInputBindingDescription vbd{};
    vbd.binding = 0;
    vbd.stride = skinned ? sizeof(GpuSkinnedVertex) : sizeof(GpuVertex);
    vbd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription vattr[6]{};
    vattr[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, pos)};
    vattr[1] = {1, 0, VK_FORMAT_R16G16_SNORM, offsetof(GpuVertex, normal)};
    vattr[2] = {2, 0, VK_FORMAT_R16G16_UNORM, offsetof(GpuVertex, uv)};
    vattr[3] = {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuVertex, color)};
    // Joint indices stay integral (_UINT, read as uvec4); weights are
    // _UNORM so the hardware hands the shader the 0..1 partition directly.
    vattr[4] = {4, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(GpuSkinnedVertex, joints)};
    vattr[5] = {5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GpuSkinnedVertex, weights)};
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = skinned ? 6u : 4u;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    // Mirrored: reflecting across a plane is an improper transform, so the
    // triangles arrive wound the other way and front faces must be culled.
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cull_none ? VK_CULL_MODE_NONE
                            : (mirrored ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;
    // Alpha-tested foliage keeps soft edges under MSAA.
    ms.alphaToCoverageEnable =
        (alpha_test && samples_ != VK_SAMPLE_COUNT_1_BIT) ? VK_TRUE : VK_FALSE;

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
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &gpci, nullptr, &p) !=
        VK_SUCCESS) {
        PTV_ERROR("scene pipeline variant {} failed to build", bits);
        return VK_NULL_HANDLE;
    }
    pipelines_[bits] = p;
    return p;
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

    // The sky is drawn AFTER the opaque geometry at the far plane with a
    // depth test, so it only shades pixels nothing covered (the old
    // sky-first order shaded the whole screen and then overdrew most of it).
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

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
    return vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &gpci,
                                     nullptr, &vignette_pipeline_) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Mesh slots
// ---------------------------------------------------------------------------

bool Renderer::upload_mesh_buffers(const void* vdata, std::size_t vbytes,
                                   const std::uint32_t* indices, std::size_t index_count,
                                   GltfGpuMesh& gm) {
    auto make_vma_buffer = [&](VkBufferUsageFlags usage, VkDeviceSize size,
                               VkBuffer& buf, VmaAllocation& alloc,
                               VmaAllocationInfo& info) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &buf, &alloc, &info) == VK_SUCCESS) {
            return true;
        }
        // Out of memory is recoverable: free what is invisible, retry once.
        if (!recover_from_allocation_failure()) return false;
        return vmaCreateBuffer(vk_->allocator(), &bci, &aci, &buf, &alloc, &info) ==
               VK_SUCCESS;
    };
    VmaAllocationInfo vinfo{}, iinfo{};
    if (!make_vma_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vbytes, gm.vb, gm.vb_alloc,
                         vinfo)) {
        PTV_ERROR("mesh: vertex buffer allocation failed ({} KB)", vbytes >> 10);
        return false;
    }
    std::memcpy(vinfo.pMappedData, vdata, vbytes);
    vmaFlushAllocation(vk_->allocator(), gm.vb_alloc, 0, VK_WHOLE_SIZE);
    const std::size_t ibytes = index_count * sizeof(std::uint32_t);
    if (!make_vma_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ibytes, gm.ib, gm.ib_alloc,
                         iinfo)) {
        PTV_ERROR("mesh: index buffer allocation failed ({} KB)", ibytes >> 10);
        vmaDestroyBuffer(vk_->allocator(), gm.vb, gm.vb_alloc);
        gm.vb = VK_NULL_HANDLE;
        return false;
    }
    std::memcpy(iinfo.pMappedData, indices, ibytes);
    vmaFlushAllocation(vk_->allocator(), gm.ib_alloc, 0, VK_WHOLE_SIZE);
    gm.index_count = static_cast<uint32_t>(index_count);
    gm.triangle_count = gm.index_count / 3;
    gm.gpu_bytes = vinfo.size + iinfo.size;
    return true;
}

int Renderer::store_mesh_slot(GltfGpuMesh&& gm) {
    resident_triangles_ += gm.triangle_count;
    // Reuse a slot freed by eviction before growing the table, so mesh_slot
    // indices stay stable and the vector does not grow without bound.
    if (!free_mesh_slots_.empty()) {
        const int slot = free_mesh_slots_.back();
        free_mesh_slots_.pop_back();
        gltf_meshes_[static_cast<size_t>(slot)] = std::move(gm);
        return slot;
    }
    const int slot = static_cast<int>(gltf_meshes_.size());
    gltf_meshes_.push_back(std::move(gm));
    return slot;
}

template <class V>
float bounding_radius(const std::vector<V>& verts, Vec3* out_center = nullptr) {
    float mn[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float mx[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& v : verts) {
        for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], v.pos[c]);
            mx[c] = std::max(mx[c], v.pos[c]);
        }
    }
    const float cx = 0.5f * (mn[0] + mx[0]);
    const float cy = 0.5f * (mn[1] + mx[1]);
    const float cz = 0.5f * (mn[2] + mx[2]);
    if (out_center != nullptr) *out_center = Vec3(cx, cy, cz);
    float r2 = 0.0f;
    for (const auto& v : verts) {
        const float dx = v.pos[0] - cx, dy = v.pos[1] - cy, dz = v.pos[2] - cz;
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    return std::sqrt(r2);
}

Renderer::SlotInfo Renderer::mesh_slot_info(int slot) const {
    SlotInfo s;
    if (slot < 0 || static_cast<size_t>(slot) >= gltf_meshes_.size()) return s;
    const GltfGpuMesh& gm = gltf_meshes_[static_cast<size_t>(slot)];
    if (gm.vb == VK_NULL_HANDLE) return s;
    s.valid = true;
    s.skinned = gm.skinned;
    s.radius = gm.local_radius;
    s.center = gm.local_center;
    s.triangles = gm.triangle_count;
    s.submeshes = static_cast<int>(gm.submeshes.size());
    for (const auto& sm : gm.submeshes) {
        if (sm.texture >= 0) ++s.textured;
        if (sm.missing_texture) ++s.missing;
    }
    return s;
}

TextureRequest Renderer::texture_request(const std::string& glb_path,
                                         const gltf_mesh_load::MeshMaterials& mats,
                                         int image_index, const std::string& label) const {
    TextureRequest req;
    const auto& img = mats.images[static_cast<size_t>(image_index)];
    req.label = label;
    if (!img.ktx2_uri.empty()) {
        req.ktx2_path = gltf_mesh_load::resolve_relative_uri(glb_path, img.ktx2_uri);
    }
    if (!img.uri.empty()) {
        req.fallback_path = gltf_mesh_load::resolve_relative_uri(glb_path, img.uri);
    }
    if (img.uri.empty() && !img.bytes.empty()) {
        req.embedded = img.bytes;
        req.embedded_hash = img.hash;
        std::string dir = glb_path;
        for (auto& c : dir) {
            if (c == '\\') c = '/';
        }
        const auto slash = dir.find_last_of('/');
        req.glb_dir = slash == std::string::npos ? std::string() : dir.substr(0, slash);
    }
    // Shared key: the same file (or the same embedded bytes) is one texture
    // no matter how many meshes and LODs use it.
    if (!req.ktx2_path.empty()) {
        req.key = req.ktx2_path;
    } else if (!req.fallback_path.empty()) {
        req.key = req.fallback_path;
    } else if (!req.embedded_hash.empty()) {
        req.key = "embedded#" + req.embedded_hash;
    }
    return req;
}

void Renderer::decode_textures(const std::string& glb_path,
                               const gltf_mesh_load::MeshMaterials& mats,
                               std::vector<DecodedTexture>& out) const {
    std::unordered_set<int> seen;
    for (const auto& sm : mats.submeshes) {
        const int img = mats.materials[static_cast<size_t>(sm.material)].base_color_image;
        if (img < 0 || !seen.insert(img).second) continue;
        TextureRequest req = texture_request(glb_path, mats, img, glb_path);
        if (req.key.empty() || textures_.has(req.key)) continue;
        DecodedTexture d;
        d.key = req.key;
        d.ok = textures_.decode(req, d.data, d.error);
        out.push_back(std::move(d));
    }
}

void Renderer::bind_materials(GltfGpuMesh& gm, const std::string& glb_path,
                              const gltf_mesh_load::MeshMaterials* mats,
                              std::vector<DecodedTexture>* decoded) {
    if (mats == nullptr) return;
    std::size_t i = 0;
    for (const auto& src : mats->submeshes) {
        GpuSubmesh& dst = gm.submeshes[i++];
        const auto& mat = mats->materials[static_cast<size_t>(src.material)];
        if (mat.alpha_mode != gltf_mesh_load::AlphaMode::Opaque) {
            // BLEND has no sorted transparent pass yet; it is drawn as MASK at
            // 0.5 (reported once below) rather than as an opaque slab.
            dst.alpha_cutoff = mat.alpha_mode == gltf_mesh_load::AlphaMode::Mask
                                   ? std::max(0.01f, mat.alpha_cutoff)
                                   : 0.5f;
            dst.cull_none = mat.double_sided;
        }
        if (mat.base_color_image < 0) continue;
        const std::string label =
            glb_path + " material '" + (mat.name.empty() ? std::string("?") : mat.name) + "'";
        TextureRequest req = texture_request(glb_path, *mats, mat.base_color_image, label);
        if (req.key.empty()) {
            PTV_ERROR("textures: MISSING {} -- the base-colour image has no uri and no "
                      "bytes (stage: runtime load; the GLB is malformed)", label);
            dst.missing_texture = true;
            textures_.note_missing();
            continue;
        }
        int id = textures_.acquire(req.key, mat.sampler);
        if (id < 0 && decoded != nullptr) {
            for (auto& d : *decoded) {
                if (d.key != req.key) continue;
                if (d.ok) {
                    const std::string source = d.data.source;
                    const auto w = d.data.width, h = d.data.height;
                    id = textures_.upload(req.key, std::move(d.data), mat.sampler);
                    d.ok = false;  // consumed
                    if (id >= 0) {
                        PTV_INFO("textures: {} -> {}x{} {} ({} KB)", req.key, w, h, source,
                                 textures_.texture_bytes(id) >> 10);
                    }
                } else if (!d.error.empty()) {
                    PTV_ERROR("textures: MISSING {} -- {} (stage: runtime load)", label,
                              d.error);
                    d.error.clear();
                }
                break;
            }
            if (id < 0) id = textures_.acquire(req.key, mat.sampler);
        }
        if (id < 0) {
            TextureData data;
            std::string err;
            if (textures_.decode(req, data, err)) {
                const std::string source = data.source;
                const auto w = data.width, h = data.height;
                id = textures_.upload(req.key, std::move(data), mat.sampler);
                if (id >= 0) {
                    PTV_INFO("textures: {} -> {}x{} {} ({} KB)", req.key, w, h, source,
                             textures_.texture_bytes(id) >> 10);
                }
            } else {
                PTV_ERROR("textures: MISSING {} -- {} (stage: runtime load)", label, err);
            }
        }
        if (id < 0) {
            dst.missing_texture = true;
            textures_.note_missing();
        } else {
            dst.texture = id;
        }
        if (dst.alpha_cutoff > 0.0f || dst.cull_none) {
            scene_pipeline((gm.skinned ? kPipeSkinned : 0u) |
                           (dst.alpha_cutoff > 0.0f ? kPipeAlphaTest : 0u) |
                           (dst.cull_none ? kPipeCullNone : 0u));
        }
    }
    bool warned_blend = false;
    for (const auto& src : mats->submeshes) {
        const auto& mat = mats->materials[static_cast<size_t>(src.material)];
        if (mat.alpha_mode == gltf_mesh_load::AlphaMode::Blend && !warned_blend) {
            PTV_WARN("textures: {} uses alphaMode BLEND; drawn as alpha-tested (cutoff 0.5) "
                     "-- sorted transparency is not implemented", glb_path);
            warned_blend = true;
        }
    }
}

int Renderer::create_mesh_slot(const std::vector<gltf_mesh_load::LitVertex>& verts,
                               const std::vector<std::uint32_t>& indices,
                               const gltf_mesh_load::MeshMaterials* mats,
                               const std::string& glb_path,
                               std::vector<DecodedTexture>* decoded) {
    if (!vk_ || verts.empty() || indices.empty()) return -1;
    GltfGpuMesh gm{};
    gm.skinned = false;
    gm.uv_xform = uv_range(verts);
    std::vector<GpuVertex> packed(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) packed[i] = pack_vertex(verts[i], gm.uv_xform);
    if (!upload_mesh_buffers(packed.data(), packed.size() * sizeof(GpuVertex), indices.data(),
                             indices.size(), gm)) {
        return -1;
    }
    gm.local_radius = bounding_radius(verts, &gm.local_center);
    if (mats != nullptr && !mats->submeshes.empty()) {
        for (const auto& sm : mats->submeshes) {
            GpuSubmesh g;
            g.first_index = sm.first_index;
            g.index_count = sm.index_count;
            gm.submeshes.push_back(g);
        }
        bind_materials(gm, glb_path, mats, decoded);
    } else {
        GpuSubmesh g;
        g.first_index = 0;
        g.index_count = gm.index_count;
        gm.submeshes.push_back(g);
    }
    return store_mesh_slot(std::move(gm));
}

int Renderer::create_mesh_slot_skinned(
    const std::vector<gltf_mesh_load::SkinnedVertex>& verts,
    const std::vector<std::uint32_t>& indices,
    const gltf_mesh_load::MeshMaterials* mats, const std::string& glb_path) {
    if (!vk_ || verts.empty() || indices.empty()) return -1;
    GltfGpuMesh gm{};
    gm.skinned = true;
    gm.uv_xform = uv_range(verts);
    std::vector<GpuSkinnedVertex> packed(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        packed[i].v = pack_vertex(verts[i], gm.uv_xform);
        std::memcpy(packed[i].joints, verts[i].joints, 4);
        std::memcpy(packed[i].weights, verts[i].weights, 4);
    }
    if (!upload_mesh_buffers(packed.data(), packed.size() * sizeof(GpuSkinnedVertex),
                             indices.data(), indices.size(), gm)) {
        return -1;
    }
    // Bind-pose bound. A pose can push vertices outside it, so animated
    // characters are culled slightly optimistically -- unless the asset
    // optimizer measured clip-swept bounds (CMesh::bounds_radius_m), which
    // the draw loop prefers.
    gm.local_radius = bounding_radius(verts, &gm.local_center);
    if (mats != nullptr && !mats->submeshes.empty()) {
        for (const auto& sm : mats->submeshes) {
            GpuSubmesh g;
            g.first_index = sm.first_index;
            g.index_count = sm.index_count;
            gm.submeshes.push_back(g);
        }
        bind_materials(gm, glb_path, mats, nullptr);
    } else {
        GpuSubmesh g;
        g.first_index = 0;
        g.index_count = gm.index_count;
        gm.submeshes.push_back(g);
    }
    return store_mesh_slot(std::move(gm));
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
        result.path = req.path;
        std::string err;
        gltf_mesh_load::LoadOptions opts;
        opts.want_image_bytes = true;
        result.ok = gltf_mesh_load::load_glb_mesh(
            req.path, req.target_scale_meters, result.verts, result.indices,
            &result.materials, err, req.unit_scale, opts);
        if (!result.ok) {
            PTV_WARN("stream: GLB load failed ({}): {}", req.path, err);
        } else {
            // Texture file reads + decodes happen here, off the render thread.
            decode_textures(req.path, result.materials, result.textures);
            // Encoded bytes are no longer needed once decoded.
            for (auto& img : result.materials.images) {
                std::vector<unsigned char>().swap(img.bytes);
            }
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
                              float priority, bool floor) {
    if (path.empty()) return;
    const std::string key = mesh_key(path, target_scale_meters, unit_scale);
    if (floor) floor_keys_.insert(key);
    if (auto it = gltf_path_to_slot_.find(key); it != gltf_path_to_slot_.end()) {
        if (floor) {
            auto& gm = gltf_meshes_[static_cast<size_t>(it->second)];
            gm.floor = true;
            gm.evictable = false;
        }
        return;
    }
    if (stream_failed_.count(key)) return;  // don't retry a broken asset forever
    // Under memory pressure only the always-resident floor may start loading.
    if (memory_pressure_ >= 2 && !floor) return;

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
    textures_.collect(frame_index_);
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
            const int slot = create_mesh_slot(result.verts, result.indices,
                                              &result.materials, result.path,
                                              &result.textures);
            if (slot >= 0) {
                auto& gm = gltf_meshes_[static_cast<size_t>(slot)];
                gm.path = result.key;
                gm.floor = floor_keys_.count(result.key) != 0;
                gm.evictable = !gm.floor;
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
    for (const auto& sm : gm.submeshes) {
        if (sm.texture >= 0) textures_.release(sm.texture);
    }
    gm = GltfGpuMesh{};
}

void Renderer::evict_to_budget() {
    // Residency is on for every tier but only acts when it has to: over the
    // byte budget from the memory model, over the triangle budget on tiers
    // that ask for it, or when MemoryGuard raised its level.
    std::uint64_t resident_bytes = textures_.stats().bytes;
    for (const auto& gm : gltf_meshes_) resident_bytes += gm.gpu_bytes;
    const std::uint64_t budget =
        static_cast<std::uint64_t>(std::max(64.0f, perf_.resident_memory_budget_mb)) *
        1024ull * 1024ull;
    const bool over_bytes = resident_bytes > budget;
    const bool over_tris = perf_.evict_when_over_budget &&
                           resident_triangles_ > perf_.resident_triangle_budget;
    if (!over_bytes && !over_tris && memory_pressure_ == 0) return;

    // Yellow: only meshes unused for ~1.5 s. Red and above: anything not
    // drawn this frame or the last, several per frame. The always-resident
    // floor (lowest LODs) is never a victim.
    const std::uint64_t min_idle = memory_pressure_ >= 2 ? 2u
                                   : memory_pressure_ == 1 ? 108u
                                                           : 1u;
    const int max_victims = memory_pressure_ >= 2 ? 8 : 1;
    for (int n = 0; n < max_victims; ++n) {
        // A frame still in flight may draw the victim, so its buffers are
        // retired and freed by draw_frame once every such frame has passed
        // its fence.
        int victim = -1;
        std::uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < gltf_meshes_.size(); ++i) {
            const auto& gm = gltf_meshes_[i];
            if (!gm.evictable || gm.floor || gm.vb == VK_NULL_HANDLE) continue;
            if (gm.last_used_frame + min_idle > frame_index_) continue;
            if (gm.last_used_frame < oldest) {
                oldest = gm.last_used_frame;
                victim = static_cast<int>(i);
            }
        }
        if (victim < 0) return;

        auto& gm = gltf_meshes_[static_cast<size_t>(victim)];
        resident_triangles_ -= std::min(resident_triangles_, gm.triangle_count);
        gltf_path_to_slot_.erase(gm.path);
        PTV_INFO("stream: evicted {} ({} tris, {:.1f} MB; resident now {} tris, pressure {})",
                 gm.path, gm.triangle_count, static_cast<double>(gm.gpu_bytes) / 1048576.0,
                 resident_triangles_, memory_pressure_);
        retired_meshes_.push_back(RetiredMesh{std::move(gm), frame_index_});
        gm = GltfGpuMesh{};
        free_mesh_slots_.push_back(victim);
        ++evictions_;
        if (memory_pressure_ == 0) {
            resident_bytes = textures_.stats().bytes;
            for (const auto& m : gltf_meshes_) resident_bytes += m.gpu_bytes;
            if (resident_bytes <= budget &&
                !(perf_.evict_when_over_budget &&
                  resident_triangles_ > perf_.resident_triangle_budget)) {
                return;
            }
        }
    }
}

bool Renderer::recover_from_allocation_failure() {
    if (!vk_) return false;
    // Rare and already a crisis, so it may stall: wait for the GPU, then free
    // every retired mesh and every streamable mesh not drawn this frame or
    // the last (never the always-resident floor), and return heap pages.
    vkDeviceWaitIdle(vk_->device());
    int freed = 0;
    for (auto& r : retired_meshes_) {
        destroy_mesh_slot(r.mesh);
        ++freed;
    }
    retired_meshes_.clear();
    for (size_t i = 0; i < gltf_meshes_.size(); ++i) {
        auto& gm = gltf_meshes_[i];
        if (!gm.evictable || gm.floor || gm.vb == VK_NULL_HANDLE) continue;
        if (gm.last_used_frame + 2 > frame_index_) continue;
        resident_triangles_ -= std::min(resident_triangles_, gm.triangle_count);
        gltf_path_to_slot_.erase(gm.path);
        destroy_mesh_slot(gm);
        free_mesh_slots_.push_back(static_cast<int>(i));
        ++freed;
        ++evictions_;
    }
    MemoryGuard::purge_heap();
    PTV_ERROR("render: GPU allocation failed; freed {} mesh(es) and purged the heap, "
              "retrying once", freed);
    return freed > 0;
}

std::uint64_t Renderer::pending_stream_bytes() {
    std::uint64_t total = 0;
    std::size_t n = 0;
    for (const auto& gm : gltf_meshes_) {
        if (gm.evictable && gm.vb != VK_NULL_HANDLE) {
            total += gm.gpu_bytes;
            ++n;
        }
    }
    const std::uint64_t avg = n ? total / n : 2ull * 1024ull * 1024ull;
    std::lock_guard<std::mutex> lock(stream_mutex_);
    return avg * stream_inflight_.size();
}

void Renderer::request_floor_lods(World& world) {
    auto view = world.registry().view<const CMesh>();
    std::size_t n = 0;
    for (auto e : view) {
        const auto& cm = view.get<const CMesh>(e);
        if (cm.lod_paths.size() < 2 || world.registry().all_of<CSkeleton>(e)) continue;
        request_stream(cm.lod_paths.back(), cm.target_scale_meters, cm.unit_scale, 1e6f,
                       /*floor=*/true);
        ++n;
    }
    if (n) PTV_INFO("stream: {} lowest-LOD meshes queued as the always-resident floor", n);
}

void Renderer::set_performance(const ScenePerformance& perf) {
    perf_ = perf;
    cull_distance_m_ = perf.cull_distance_m;
    lod_bias_ = 0.0f;
}

bool Renderer::create_primitive_meshes() {
    std::vector<gltf_mesh_load::LitVertex> v;
    std::vector<uint32_t> idx;

    v.clear(); idx.clear(); make_cube(v, idx);
    prim_slot_cube_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_sphere(v, idx);
    prim_slot_sphere_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_cylinder(v, idx);
    prim_slot_cylinder_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_plane(v, idx);
    prim_slot_plane_ = create_mesh_slot(v, idx);
    v.clear(); idx.clear(); make_ramp(v, idx);
    prim_slot_ramp_ = create_mesh_slot(v, idx);

    return prim_slot_cube_ >= 0 && prim_slot_sphere_ >= 0 && prim_slot_cylinder_ >= 0 &&
           prim_slot_plane_ >= 0 && prim_slot_ramp_ >= 0;
}

bool Renderer::create_msaa_color_image() {
    if (samples_ == VK_SAMPLE_COUNT_1_BIT) return true;  // no MSAA target needed
    const uint32_t layers = multiview_ ? 2u : 1u;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = color_format_;
    ici.extent = {eye_width_, eye_height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = samples_;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Transient: the samples live in tile memory and are resolved before the
    // pass ends, so with lazily allocated memory (Meta: "Take advantage of
    // lazy allocated buffers") this image never needs backing RAM at all.
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
    VmaAllocationInfo info{};
    if (vmaCreateImage(vk_->allocator(), &ici, &aci, &msaa_color_image_,
                       &msaa_color_alloc_, &info) != VK_SUCCESS) {
        // Not every GPU offers lazily allocated memory (desktop GPUs do not).
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(vk_->allocator(), &ici, &aci, &msaa_color_image_,
                           &msaa_color_alloc_, &info) != VK_SUCCESS) {
            PTV_ERROR("vmaCreateImage(msaa color) failed");
            return false;
        }
        target_bytes_ += info.size;
    } else {
        PTV_INFO("Renderer: MSAA color is lazily allocated (no backing memory)");
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = msaa_color_image_;
    vci.viewType = multiview_ ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = layers;
    return vkCreateImageView(vk_->device(), &vci, nullptr, &msaa_color_view_) ==
           VK_SUCCESS;
}

bool Renderer::create_depth_image() {
    const uint32_t layers = multiview_ ? 2u : 1u;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depth_format_;
    ici.extent = {eye_width_, eye_height_, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = samples_;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Depth is cleared on load and never stored: transient, like the MSAA
    // colour, so it too can live entirely in tile memory.
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
    VmaAllocationInfo info{};
    if (vmaCreateImage(vk_->allocator(), &ici, &aci, &depth_image_,
                       &depth_alloc_, &info) != VK_SUCCESS) {
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateImage(vk_->allocator(), &ici, &aci, &depth_image_,
                           &depth_alloc_, &info) != VK_SUCCESS) {
            PTV_ERROR("vmaCreateImage(depth) failed");
            return false;
        }
        target_bytes_ += info.size;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depth_image_;
    vci.viewType = multiview_ ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depth_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = layers;
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
    const int keep_slot = frame_slot_;
    for (int s = 0; s < kMaxFramesInFlight; ++s) {
        select_frame_slot(s);
        if (vkAllocateCommandBuffers(vk_->device(), &ai, &cmd_) != VK_SUCCESS) {
            return false;
        }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(vk_->device(), &fci, nullptr, &frame_fence_) !=
            VK_SUCCESS) {
            return false;
        }
    }
    select_frame_slot(keep_slot);

    // GPU frame timer. Optional: a queue without timestamp support just
    // reports 0 and the governor falls back to wall time.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vk_->physical_device(), &props);
    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_->physical_device(), &qc,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> qp(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(vk_->physical_device(), &qc,
                                             qp.data());
    const uint32_t fam = vk_->graphics_queue_family();
    const uint32_t valid_bits = fam < qc ? qp[fam].timestampValidBits : 0u;
    if (valid_bits > 0 && props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2 * kMaxFramesInFlight;
        if (vkCreateQueryPool(vk_->device(), &qpi, nullptr, &timestamp_pool_) ==
            VK_SUCCESS) {
            timestamp_period_ns_ = props.limits.timestampPeriod;
            timestamp_mask_ = valid_bits >= 64 ? ~0ull
                                               : ((1ull << valid_bits) - 1ull);
        } else {
            timestamp_pool_ = VK_NULL_HANDLE;
        }
    }
    if (timestamp_pool_ == VK_NULL_HANDLE) {
        PTV_WARN("Renderer: GPU timestamps unavailable; gpu time reads 0");
    }
    return true;
}

VkImageView Renderer::get_or_create_color_view(VkImage img) {
    const std::uint64_t key = image_key(img, 0);
    auto it = color_views_.find(key);
    if (it != color_views_.end()) return it->second;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = multiview_ ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = color_format_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = multiview_ ? 2u : 1u;
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vk_->device(), &vci, nullptr, &v) != VK_SUCCESS) {
        PTV_ERROR("vkCreateImageView (swapchain color) failed");
        return VK_NULL_HANDLE;
    }
    color_views_.emplace(key, v);
    return v;
}

VkFramebuffer Renderer::get_or_create_framebuffer(const EyeTarget& eye) {
    const std::uint64_t key = image_key(eye.image, 0);
    auto it = framebuffers_.find(key);
    if (it != framebuffers_.end()) return it->second;
    VkImageView swap_v = get_or_create_color_view(eye.image);
    if (!swap_v) return VK_NULL_HANDLE;

    // MSAA: [msaa_color, depth, resolve(swapchain)]. Non-MSAA:
    // [swapchain, depth]. FFR appends the density map. The MSAA color + depth
    // are shared across frames; only the swapchain image differs.
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;
    std::array<VkImageView, 4> atts{};
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
    if (fdm_) {
        if (eye.density_map == VK_NULL_HANDLE) {
            PTV_ERROR("Renderer: FFR render pass but no fragment density map image");
            return VK_NULL_HANDLE;
        }
        const std::uint64_t dkey = image_key(eye.density_map, 1);
        VkImageView dv = VK_NULL_HANDLE;
        auto dit = density_views_.find(dkey);
        if (dit != density_views_.end()) {
            dv = dit->second;
        } else {
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = eye.density_map;
            vci.viewType = multiview_ ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8_UNORM;
            vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vci.subresourceRange.levelCount = 1;
            vci.subresourceRange.layerCount = multiview_ ? 2u : 1u;
            if (vkCreateImageView(vk_->device(), &vci, nullptr, &dv) != VK_SUCCESS) {
                PTV_ERROR("vkCreateImageView (density map) failed");
                return VK_NULL_HANDLE;
            }
            density_views_.emplace(dkey, dv);
        }
        atts[att_count++] = dv;
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
    framebuffers_.emplace(key, fb);
    return fb;
}

Renderer::MemoryStats Renderer::memory_stats() const {
    MemoryStats m;
    for (const auto& gm : gltf_meshes_) m.mesh_bytes += gm.gpu_bytes;
    m.texture_bytes = textures_.stats().bytes;
    m.target_bytes = target_bytes_;
    m.frame_bytes = static_cast<std::uint64_t>(joint_ssbo_capacity_) * sizeof(Mat4) +
                    static_cast<std::uint64_t>(instance_capacity_) * sizeof(GpuInstance) +
                    sizeof(GpuFrame);
    return m;
}

void Renderer::draw_frame(const std::array<EyeTarget, 2>& eyes,
                          const std::vector<DrawItem>& items,
                          const RenderEnv& env) {
    // Wait for the PREVIOUS frame's GPU work to finish before touching the
    // command buffer or the per-frame buffers again. The fence is created
    // signaled so the very first frame returns immediately.
    using fence_clock = std::chrono::steady_clock;
    select_frame_slot(frames_in_flight_ > 1
                          ? static_cast<int>(frame_index_ % static_cast<std::uint64_t>(
                                                                frames_in_flight_))
                          : 0);
    const std::uint32_t ts_base = static_cast<std::uint32_t>(frame_slot_) * 2u;
    const auto wait0 = fence_clock::now();
    vkWaitForFences(vk_->device(), 1, &frame_fence_, VK_TRUE, UINT64_MAX);
    float fence_wait_ms = std::chrono::duration<float, std::milli>(
                              fence_clock::now() - wait0)
                              .count();
    vkResetFences(vk_->device(), 1, &frame_fence_);
    FrameSlot& slot_state = frame_slots_[static_cast<size_t>(frame_slot_)];
    auto read_gpu_time = [this, ts_base]() {
        std::uint64_t ts[2]{0, 0};
        if (vkGetQueryPoolResults(vk_->device(), timestamp_pool_, ts_base, 2, sizeof(ts), ts,
                                  sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const std::uint64_t t0 = ts[0] & timestamp_mask_;
            const std::uint64_t t1 = ts[1] & timestamp_mask_;
            if (t1 > t0) {
                last_gpu_ms_ = static_cast<float>(static_cast<double>(t1 - t0) *
                                                  timestamp_period_ns_ * 1e-6);
            }
        }
    };
    // Two frames in flight: this slot's previous frame has just finished, so
    // its timestamps are the newest complete GPU time.
    if (frames_in_flight_ > 1 && timestamp_pool_ && slot_state.timestamps_written) {
        read_gpu_time();
    }
    // Evicted meshes are freed only once every frame that could draw them
    // has passed its fence.
    if (!retired_meshes_.empty()) {
        const std::uint64_t safe = static_cast<std::uint64_t>(frames_in_flight_);
        auto it = retired_meshes_.begin();
        while (it != retired_meshes_.end()) {
            if (frame_index_ >= it->frame + safe) {
                destroy_mesh_slot(it->mesh);
                it = retired_meshes_.erase(it);
            } else {
                ++it;
            }
        }
    }
    fx_.set_frame_slot(frame_slot_);
    text_.set_frame_slot(frame_slot_);
    textures_.set_frame(frame_index_);
    std::uint32_t draw_calls = 0, pipeline_binds = 0, descriptor_binds = 0;
    std::uint32_t missing_draws = 0;

    // ---- Frame uniform block (both eyes + environment) ---------------------
    if (frame_ubo_mapped_) {
        Vec3 sun = env.sun_direction;
        if (glm::length(sun) < 1e-5f) sun = Vec3{0.0f, -1.0f, 0.0f};
        sun = glm::normalize(sun);

        GpuFrame g{};
        for (int e = 0; e < 2; ++e) {
            const Mat4 vp = eyes[static_cast<size_t>(e)].projection *
                            eyes[static_cast<size_t>(e)].view;
            g.view_proj[e] = vp;
            g.inv_view_proj[e] = glm::inverse(vp);
            g.inv_proj[e] = glm::inverse(eyes[static_cast<size_t>(e)].projection);
            const Vec3 cam = Vec3(glm::inverse(eyes[static_cast<size_t>(e)].view)[3]);
            g.camera_pos[e][0] = cam.x;
            g.camera_pos[e][1] = cam.y;
            g.camera_pos[e][2] = cam.z;
        }
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
        g.misc[0] = static_cast<float>(count);
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
        std::memcpy(frame_ubo_mapped_, &g, sizeof(g));
        // Make the write visible to the device before submit. No-op on the
        // coherent host memory VMA usually picks; correct if it doesn't.
        vmaFlushAllocation(vk_->allocator(), frame_ubo_alloc_, 0, sizeof(g));
    }

    // ---- Joint palettes (shared by both eyes) -----------------------------
    // Growing the buffer is safe here: we are past the fence, so no command
    // buffer is still reading the old allocation.
    if (!joint_palettes_.empty()) {
        if (ensure_joint_buffer(
                static_cast<std::uint32_t>(joint_palettes_.size())) &&
            joint_ssbo_mapped_) {
            const size_t bytes = joint_palettes_.size() * sizeof(Mat4);
            std::memcpy(joint_ssbo_mapped_, joint_palettes_.data(), bytes);
            vmaFlushAllocation(vk_->allocator(), joint_ssbo_alloc_, 0, bytes);
        }
    }

    // ---- Sorted, instanced draw list --------------------------------------
    // One record per (item, submesh). Sorted by pipeline, then material set,
    // then mesh, so state changes happen once per run and identical meshes
    // with identical materials collapse into one instanced draw.
    struct Record {
        std::uint32_t pipe;
        VkDescriptorSet set;
        int slot;
        std::uint32_t submesh;
        std::uint32_t item;
    };
    std::vector<Record> records;
    records.reserve(items.size() + 16);
    for (std::uint32_t i = 0; i < items.size(); ++i) {
        const DrawItem& it = items[i];
        const int slot = it.mesh_slot >= 0 ? it.mesh_slot : prim_slot_cube_;
        if (slot < 0 || static_cast<size_t>(slot) >= gltf_meshes_.size()) continue;
        const GltfGpuMesh& gm = gltf_meshes_[static_cast<size_t>(slot)];
        if (gm.vb == VK_NULL_HANDLE) continue;
        // Only skin when the item asked for it AND the mesh really is
        // skinned: drawing static vertices through the skinned pipeline would
        // read joint indices out of the next vertex.
        const bool skinned = gm.skinned && it.joint_base >= 0 && it.joint_count > 0;
        if (gm.skinned && !skinned) continue;
        for (std::uint32_t s = 0; s < gm.submeshes.size(); ++s) {
            const GpuSubmesh& sm = gm.submeshes[s];
            std::uint32_t pipe = (skinned ? kPipeSkinned : 0u) |
                                 (it.mirrored ? kPipeMirrored : 0u) |
                                 (sm.alpha_cutoff > 0.0f ? kPipeAlphaTest : 0u) |
                                 (sm.cull_none ? kPipeCullNone : 0u);
            VkDescriptorSet set = textures_.white_set();
            if (sm.texture >= 0) {
                set = textures_.descriptor(sm.texture);
            } else if (sm.missing_texture) {
                ++missing_draws;
                if (texture_diagnostics_) set = textures_.missing_set();
            }
            records.push_back(Record{pipe, set, slot, s, i});
        }
    }
    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        if (a.pipe != b.pipe) return a.pipe < b.pipe;
        if (a.set != b.set) return a.set < b.set;
        if (a.slot != b.slot) return a.slot < b.slot;
        if (a.submesh != b.submesh) return a.submesh < b.submesh;
        return a.item < b.item;
    });
    const auto record_count = static_cast<std::uint32_t>(records.size());
    if (record_count > 0 && ensure_instance_buffer(record_count) && instance_ssbo_mapped_) {
        auto* dst = static_cast<GpuInstance*>(instance_ssbo_mapped_);
        for (std::uint32_t r = 0; r < record_count; ++r) {
            const Record& rec = records[r];
            const DrawItem& it = items[rec.item];
            const GltfGpuMesh& gm = gltf_meshes_[static_cast<size_t>(rec.slot)];
            const GpuSubmesh& sm = gm.submeshes[rec.submesh];
            GpuInstance in{};
            in.model = it.model;
            in.color = it.color;
            in.uv_xform = gm.uv_xform;
            const bool skinned = (rec.pipe & kPipeSkinned) != 0;
            in.params = Vec4(it.material_id,
                             skinned ? static_cast<float>(it.joint_base) : 0.0f,
                             sm.alpha_cutoff, 0.0f);
            dst[r] = in;
        }
        vmaFlushAllocation(vk_->allocator(), instance_ssbo_alloc_, 0,
                           static_cast<VkDeviceSize>(record_count) * sizeof(GpuInstance));
    }

    // Text and FX vertex data (shared by both eyes); the previous frame's
    // fence has passed, so nothing in flight still reads those buffers.
    text_.prepare(text_items_);
    text_items_.clear();
    {
        Vec3 cam{0.0f};
        for (const auto& e : eyes) cam += Vec3(glm::inverse(e.view)[3]);
        fx_.prepare(fx_items_, cam * 0.5f, fx_budget_);
        fx_items_.clear();
    }

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    if (timestamp_pool_) {
        vkCmdResetQueryPool(cmd_, timestamp_pool_, ts_base, 2);
        // BOTTOM_OF_PIPE: written once all earlier work on the queue has
        // finished, so with two frames in flight the delta is this frame's
        // own execution and not time spent queued behind the previous one.
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            timestamp_pool_, ts_base);
    }

    // Multiview: one pass draws both eyes (layers 0 and 1 of one image).
    // Otherwise one pass per eye, the shaders reading view_base.
    const int passes = multiview_ ? 1 : 2;
    for (int pass = 0; pass < passes; ++pass) {
        const EyeTarget& eye = eyes[static_cast<size_t>(pass)];
        VkFramebuffer fb = get_or_create_framebuffer(eye);
        if (!fb) continue;

        std::array<VkClearValue, 4> clears{};
        clears[0].color = {{env.sky_horizon.x, env.sky_horizon.y,
                            env.sky_horizon.z, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        clears[2].color = clears[0].color;
        uint32_t clear_count = samples_ != VK_SAMPLE_COUNT_1_BIT ? 3u : 2u;
        if (fdm_) ++clear_count;

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

        PushConstants pcs{};
        pcs.view_base = multiview_ ? 0u : static_cast<std::uint32_t>(pass);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                &frame_set_, 0, nullptr);
        ++descriptor_binds;
        vkCmdPushConstants(cmd_, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pcs), &pcs);

        // ---- Opaque geometry, sorted and instanced ------------------------
        std::uint32_t bound_pipe = UINT32_MAX;
        VkDescriptorSet bound_set = VK_NULL_HANDLE;
        int bound_slot = -1;
        for (std::uint32_t r = 0; r < record_count;) {
            const Record& rec = records[r];
            std::uint32_t run = 1;
            while (r + run < record_count && records[r + run].pipe == rec.pipe &&
                   records[r + run].set == rec.set && records[r + run].slot == rec.slot &&
                   records[r + run].submesh == rec.submesh) {
                ++run;
            }
            if (rec.pipe != bound_pipe) {
                VkPipeline p = scene_pipeline(rec.pipe);
                if (p == VK_NULL_HANDLE) {
                    r += run;
                    continue;
                }
                vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                bound_pipe = rec.pipe;
                ++pipeline_binds;
            }
            if (rec.set != bound_set) {
                vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline_layout_, 1, 1, &rec.set, 0, nullptr);
                bound_set = rec.set;
                ++descriptor_binds;
            }
            const GltfGpuMesh& gm = gltf_meshes_[static_cast<size_t>(rec.slot)];
            if (rec.slot != bound_slot) {
                VkDeviceSize voff = 0;
                vkCmdBindVertexBuffers(cmd_, 0, 1, &gm.vb, &voff);
                vkCmdBindIndexBuffer(cmd_, gm.ib, 0, VK_INDEX_TYPE_UINT32);
                bound_slot = rec.slot;
            }
            const GpuSubmesh& sm = gm.submeshes[rec.submesh];
            vkCmdDrawIndexed(cmd_, sm.index_count, run, sm.first_index, 0, r);
            ++draw_calls;
            r += run;
        }

        // ---- Sky: after opaque geometry, only where nothing was drawn ----
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, sky_pipeline_);
        ++pipeline_binds;
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        ++draw_calls;

        // ---- Effects, then world-space text -----------------------------
        const std::array<Mat4, 2> vps = {
            multiview_ ? eyes[0].projection * eyes[0].view : eye.projection * eye.view,
            multiview_ ? eyes[1].projection * eyes[1].view : eye.projection * eye.view};
        fx_.record(cmd_, vps);
        text_.record(cmd_, vps);

        // ---- Comfort vignette: last, over everything --------------------
        if (vignette_pipeline_ != VK_NULL_HANDLE && vignette_opacity_ > 0.002f) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              vignette_pipeline_);
            constexpr float kDeg = 3.14159265f / 180.0f;
            PushConstants vpc{};
            vpc.view_base = pcs.view_base;
            vpc.p0 = Vec4(vignette_clear_deg_ * kDeg, vignette_feather_deg_ * kDeg,
                          std::clamp(vignette_opacity_, 0.0f, 1.0f), 0.0f);
            vpc.p1 = Vec4(vignette_color_, 1.0f);
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    0, 1, &frame_set_, 0, nullptr);
            vkCmdPushConstants(cmd_, pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(vpc), &vpc);
            vkCmdDraw(cmd_, 3, 1, 0, 0);
            ++draw_calls;
        }
        vkCmdEndRenderPass(cmd_);
    }
    if (timestamp_pool_) {
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            timestamp_pool_, ts_base + 1u);
        slot_state.timestamps_written = true;
    }
    vkEndCommandBuffer(cmd_);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, frame_fence_);
    stats_.draw_calls = draw_calls;
    stats_.pipeline_binds = pipeline_binds;
    stats_.descriptor_binds = descriptor_binds;
    stats_.instances = record_count;
    stats_.missing_texture_draws = missing_draws;

    // Two frames in flight (Quest): return as soon as the work is queued. The
    // OpenXR runtime orders its composition after work already submitted to
    // this queue, and the next frame's CPU work overlaps this frame's GPU.
    if (frames_in_flight_ > 1) {
        last_fence_wait_ms_ = fence_wait_ms;
        return;
    }

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
    const auto wait1 = fence_clock::now();
    vkWaitForFences(vk_->device(), 1, &frame_fence_, VK_TRUE, UINT64_MAX);
    fence_wait_ms += std::chrono::duration<float, std::milli>(
                         fence_clock::now() - wait1)
                         .count();
    last_fence_wait_ms_ = fence_wait_ms;
    if (timestamp_pool_) read_gpu_time();
}

void Renderer::destroy_gltf_meshes() {
    if (!vk_) return;
    for (auto& m : gltf_meshes_) destroy_mesh_slot(m);
    gltf_meshes_.clear();
    gltf_path_to_slot_.clear();
    inline_mesh_slots_.clear();
    free_mesh_slots_.clear();
    entity_lod_.clear();
    resident_triangles_ = 0;
    // Built-in primitive meshes lived in gltf_meshes_; their slots are now
    // invalid.
    prim_slot_cube_ = prim_slot_sphere_ = prim_slot_cylinder_ = prim_slot_plane_ =
        prim_slot_ramp_ = -1;
}

int Renderer::region_of(const Vec3& p) const {
    for (size_t i = 0; i < regions_.size(); ++i) {
        const RegionBox& r = regions_[i];
        if (p.x >= r.min.x - 0.25f && p.x <= r.max.x + 0.25f && p.z >= r.min.z - 0.25f &&
            p.z <= r.max.z + 0.25f && p.y >= r.floor_y - 0.5f && p.y <= r.floor_y + 4.5f) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Renderer::build_static_batches(World& world, const std::vector<NavRegionSpec>& regions,
                                    const std::vector<NavLinkSpec>& links) {
    // A previous scene's batches: freed once no frame in flight draws them.
    for (auto& b : batches_) {
        if (b.slot >= 0 && static_cast<size_t>(b.slot) < gltf_meshes_.size()) {
            retired_meshes_.push_back(
                RetiredMesh{std::move(gltf_meshes_[static_cast<size_t>(b.slot)]), frame_index_});
            gltf_meshes_[static_cast<size_t>(b.slot)] = GltfGpuMesh{};
            free_mesh_slots_.push_back(b.slot);
        }
    }
    batches_.clear();
    dissolve_warned_ = false;

    // Rooms: every region, with the closed ones (compiler-verified walls on
    // all four sides) the only ones whose contents may be hidden.
    regions_.clear();
    region_links_.clear();
    std::unordered_map<std::string, int> region_index;
    for (const auto& r : regions) {
        RegionBox rb{r.min, r.max, r.floor_y, false};
        rb.closed = perf_.room_culling &&
                    std::find(perf_.rooms.begin(), perf_.rooms.end(), r.id) != perf_.rooms.end();
        region_index.emplace(r.id, static_cast<int>(regions_.size()));
        regions_.push_back(rb);
    }
    region_links_.resize(regions_.size());
    for (const auto& l : links) {
        auto a = region_index.find(l.from);
        auto b = region_index.find(l.to);
        if (a == region_index.end() || b == region_index.end()) continue;
        region_links_[static_cast<size_t>(a->second)].push_back(b->second);
        region_links_[static_cast<size_t>(b->second)].push_back(a->second);
    }
    room_visible_.assign(regions_.size(), 1);
    request_floor_lods(world);

    auto& reg = world.registry();
    const float cell = std::max(2.0f, perf_.static_batch_cell_m);
    struct Group {
        std::vector<gltf_mesh_load::LitVertex> verts;
        std::vector<std::uint32_t> idx;
        std::vector<std::uint32_t> members;
        Vec3 lo{1e30f};
        Vec3 hi{-1e30f};
        int room{-1};
        float material_id{0.0f};
    };
    std::map<std::tuple<int, int, int, int>, Group> groups;
    std::size_t candidates = 0;
    std::vector<gltf_mesh_load::LitVertex> uv;
    std::vector<std::uint32_t> ui;
    auto view = reg.view<CStaticBatch, const CTransform>();
    for (auto e : view) {
        auto& sb = view.get<CStaticBatch>(e);
        sb.batch = -1;
        const Transform& t = view.get<const CTransform>(e).t;
        const int rg = region_of(t.position + Vec3(0.0f, 0.5f, 0.0f));
        sb.room = (rg >= 0 && regions_[static_cast<size_t>(rg)].closed) ? rg : -1;
        if (!perf_.static_batching || reg.all_of<CMesh>(e) || reg.all_of<CHighlight>(e)) continue;
        render_tags::TagVisuals visuals{};
        if (const auto* tag = reg.try_get<CTag>(e)) {
            visuals = render_tags::resolve(tag->tags);
            if (visuals.always_hidden) continue;
        }
        Mat4 model = t.matrix();
        Vec4 tint{1.0f};
        uv.clear();
        ui.clear();
        if (const auto* prim = reg.try_get<CPrimitive>(e)) {
            if (prim->kind == "sphere") make_sphere(uv, ui);
            else if (prim->kind == "cylinder") make_cylinder(uv, ui);
            else if (prim->kind == "plane") make_plane(uv, ui);
            else if (prim->kind == "ramp") make_ramp(uv, ui);
            else make_cube(uv, ui);
            model = model * glm::scale(Mat4(1.0f), prim->size);
            tint = Vec4(prim->color, 1.0f);
        } else if (const auto* im = reg.try_get<CInlineMesh>(e)) {
            const size_t vc = im->positions.size() / 3;
            if (vc < 3 || im->indices.size() < 3) continue;
            uv.resize(vc);
            const bool have_n = im->normals.size() == im->positions.size();
            for (size_t i = 0; i < vc; ++i) {
                auto& v = uv[i];
                v = gltf_mesh_load::LitVertex{};
                for (int k = 0; k < 3; ++k) {
                    v.pos[k] = im->positions[i * 3 + static_cast<size_t>(k)];
                    v.normal[k] = have_n ? im->normals[i * 3 + static_cast<size_t>(k)] : 0.0f;
                }
                v.color[0] = im->color.x;
                v.color[1] = im->color.y;
                v.color[2] = im->color.z;
                v.color[3] = 1.0f;
            }
            ui = im->indices;
            if (!have_n) {
                for (size_t i = 0; i + 2 < ui.size(); i += 3) {
                    const auto a = ui[i], b = ui[i + 1], c = ui[i + 2];
                    if (a >= vc || b >= vc || c >= vc) continue;
                    const Vec3 pa{uv[a].pos[0], uv[a].pos[1], uv[a].pos[2]};
                    const Vec3 pb{uv[b].pos[0], uv[b].pos[1], uv[b].pos[2]};
                    const Vec3 pc{uv[c].pos[0], uv[c].pos[1], uv[c].pos[2]};
                    const Vec3 n = glm::cross(pb - pa, pc - pa);
                    for (auto id : {a, b, c}) {
                        uv[id].normal[0] += n.x;
                        uv[id].normal[1] += n.y;
                        uv[id].normal[2] += n.z;
                    }
                }
            }
        } else {
            continue;
        }
        ++candidates;
        const Vec3 c = Vec3(model[3]);
        const auto key = std::make_tuple(static_cast<int>(std::floor(c.x / cell)),
                                         static_cast<int>(std::floor(c.z / cell)), sb.room,
                                         visuals.is_ground ? 1 : 0);
        Group& g = groups[key];
        g.room = sb.room;
        g.material_id = visuals.is_ground ? 1.0f : 0.0f;
        const Mat3 nm = glm::transpose(glm::inverse(Mat3(model)));
        const auto base = static_cast<std::uint32_t>(g.verts.size());
        for (auto v : uv) {
            const Vec3 p = Vec3(model * Vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f));
            Vec3 n = nm * Vec3(v.normal[0], v.normal[1], v.normal[2]);
            const float nl = glm::length(n);
            n = nl > 1e-8f ? n / nl : Vec3(0.0f, 1.0f, 0.0f);
            v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
            v.normal[0] = n.x; v.normal[1] = n.y; v.normal[2] = n.z;
            for (int k = 0; k < 4; ++k) v.color[k] *= tint[k];
            g.lo = glm::min(g.lo, p);
            g.hi = glm::max(g.hi, p);
            g.verts.push_back(v);
        }
        for (auto i : ui) g.idx.push_back(base + i);
        g.members.push_back(static_cast<std::uint32_t>(e));
    }

    std::size_t merged = 0;
    for (auto& [key, g] : groups) {
        if (g.members.size() < 2) continue;  // nothing to merge with
        const int slot = create_mesh_slot(g.verts, g.idx);
        if (slot < 0) continue;
        StaticBatch b{};
        b.slot = slot;
        b.center = 0.5f * (g.lo + g.hi);
        b.radius = 0.5f * glm::length(g.hi - g.lo);
        b.room = g.room;
        b.material_id = g.material_id;
        b.triangles = static_cast<std::uint32_t>(g.idx.size() / 3);
        const int bi = static_cast<int>(batches_.size());
        for (auto m : g.members) {
            const auto ent = static_cast<entt::entity>(m);
            reg.get<CStaticBatch>(ent).batch = bi;
            b.members.emplace_back(m, reg.get<CTransform>(ent).t);
        }
        merged += g.members.size();
        batches_.push_back(std::move(b));
    }
    std::size_t closed = 0;
    for (const auto& r : regions_) closed += r.closed ? 1 : 0;
    PTV_INFO("render: {} static batches from {} of {} static entities (cell {:.0f} m); "
             "room culling {} ({} closed rooms of {} regions)",
             batches_.size(), merged, candidates, cell,
             perf_.room_culling && closed > 0 ? "on" : "off", closed, regions_.size());
}

void Renderer::check_static_batches(World& world) {
    auto& reg = world.registry();
    for (size_t bi = 0; bi < batches_.size(); ++bi) {
        StaticBatch& b = batches_[bi];
        if (!b.valid) continue;
        bool moved = false;
        for (const auto& [m, snap] : b.members) {
            const auto ent = static_cast<entt::entity>(m);
            const auto* ct = reg.valid(ent) ? reg.try_get<CTransform>(ent) : nullptr;
            if (ct == nullptr || glm::length(ct->t.position - snap.position) > 1e-4f ||
                std::abs(glm::dot(ct->t.rotation, snap.rotation)) < 0.99999f ||
                std::abs(ct->t.scale - snap.scale) > 1e-5f) {
                moved = true;
                break;
            }
        }
        if (!moved) continue;
        // Something the compiler judged static moved (or was removed): stop
        // drawing the merged copy and let every member draw itself again.
        b.valid = false;
        for (const auto& [m, snap] : b.members) {
            const auto ent = static_cast<entt::entity>(m);
            if (reg.valid(ent)) {
                if (auto* sb = reg.try_get<CStaticBatch>(ent)) sb->batch = -1;
            }
        }
        if (!dissolve_warned_) {
            PTV_WARN("render: static batch {} dissolved: a member moved or was removed "
                     "(its entities now draw individually)", bi);
            dissolve_warned_ = true;
        }
    }
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
                               gltf_mesh_load::SkeletonData& out_skeleton,
                               const gltf_mesh_load::ModelFrame* frame) {
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
    gltf_mesh_load::MeshMaterials mats;
    gltf_mesh_load::LoadOptions opts;
    opts.want_image_bytes = true;
    std::string err;
    if (!gltf_mesh_load::load_glb_skinned(resolved_path, target_scale_meters,
                                          verts, indices, out_skeleton, err,
                                          frame, &mats, opts)) {
        PTV_WARN("skinned GLB load failed ({}): {}", resolved_path, err);
        return -1;
    }

    const float key_unit =
        (frame != nullptr && frame->valid) ? frame->unit_scale : 0.0f;
    const std::string key =
        mesh_key(resolved_path, target_scale_meters, key_unit);
    auto it = gltf_path_to_slot_.find(key);
    if (it != gltf_path_to_slot_.end()) {
        gltf_meshes_[static_cast<size_t>(it->second)].last_used_frame =
            frame_index_;
        return it->second;
    }

    const int slot = create_mesh_slot_skinned(verts, indices, &mats, resolved_path);
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
    gltf_mesh_load::MeshMaterials mats;
    gltf_mesh_load::LoadOptions opts;
    opts.want_image_bytes = true;
    std::string err;
    if (!gltf_mesh_load::load_glb_mesh(resolved_path, target_scale_meters, verts, indices,
                                       &mats, err, 0.0f, opts)) {
        PTV_ERROR("GLB load failed ({}): {}", resolved_path, err);
        return -1;
    }
    if (verts.empty() || indices.empty()) return -1;

    const int slot = create_mesh_slot(verts, indices, &mats, resolved_path);
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

    /** Conservative sphere test: false only when definitely outside. The far
     *  plane is ignored: distance culling owns that decision. */
    bool intersects_sphere(const Vec3& c, float r) const {
        if (!valid) return true;
        for (int i = 0; i < 5; ++i) {
            const auto& pl = planes[i];
            if (pl.x * c.x + pl.y * c.y + pl.z * c.z + pl.w < -r) return false;
        }
        return true;
    }
};

/** Union of the two eye frustums: a sphere is visible when either eye sees
 *  it. Exact per eye, unlike one widened frustum from the head centre. */
struct StereoFrustum {
    Frustum eye[2];
    Frustum fallback;
    bool stereo{false};

    bool intersects_sphere(const Vec3& c, float r) const {
        if (stereo) return eye[0].intersects_sphere(c, r) || eye[1].intersects_sphere(c, r);
        return fallback.intersects_sphere(c, r);
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

void Renderer::note_player_draw(World& world, std::uint32_t entity, const Vec3& pos) {
    if (player_points_.empty()) return;
    bool near = false;
    for (const Vec3& p : player_points_) {
        if (glm::length(pos - p) <= player_radius_) {
            near = true;
            break;
        }
    }
    if (!near) return;
    const auto is_allowed = [&](std::uint32_t id) {
        return std::find(player_allowed_.begin(), player_allowed_.end(), id) !=
               player_allowed_.end();
    };
    if (is_allowed(entity)) return;
    const auto e = static_cast<entt::entity>(entity);
    // Characters (a companion's hand brushing yours) and what the player can
    // pick up legitimately sit at a hand.
    if (world.registry().all_of<CSkeleton>(e)) return;
    if (const auto* tag = world.registry().try_get<CTag>(e)) {
        for (const auto& t : tag->tags) {
            if (t == "grabbable" || t == "player_avatar" || t == "player_hand") return;
        }
    }
    // Worn or carried by something the player owns (a holstered pistol).
    std::uint32_t parent = entity;
    for (int depth = 0; depth < 3; ++depth) {
        const auto* att = world.registry().try_get<CAttachment>(static_cast<entt::entity>(parent));
        if (att == nullptr) break;
        parent = static_cast<std::uint32_t>(att->parent);
        if (is_allowed(parent)) return;
    }
    ++stats_.player_draws;
    if (player_draw_names_.size() < 4) {
        const std::string* id = world.id_of(e);
        player_draw_names_.push_back(id != nullptr ? *id : std::to_string(entity));
    }
}

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

    StereoFrustum frustum;
    if (view_info.eyes_valid) {
        frustum.eye[0].build(view_info.eye_view_proj[0]);
        frustum.eye[1].build(view_info.eye_view_proj[1]);
        frustum.stereo = true;
    } else if (view_info.valid) {
        frustum.fallback.build(view_info.view_proj);
    }
    const bool can_cull = view_info.valid || view_info.eyes_valid;
    const Vec3 cam = view_info.camera_pos;

    // Pixels per radian of one eye: turns a size or an error in metres at a
    // distance into pixels on screen.
    const float px_per_rad =
        view_info.viewport_height_px / std::max(0.2f, view_info.fov_y);
    auto projected_px = [px_per_rad](float metres, float dist) {
        return metres * px_per_rad / std::max(dist, 0.05f);
    };

    // A static entity that moved dissolves its batch (checked twice a second).
    if (!batches_.empty() && frame_index_ % 36 == 0) check_static_batches(world);

    // Speed-aware prefetch: where the player will be in 1.5 s, from the
    // smoothed camera motion. Off under any memory pressure.
    if (can_cull) {
        if (have_prev_cam_) {
            const Vec3 d = cam - prev_cam_;
            if (glm::length(d) < 2.0f) cam_velocity_ = glm::mix(cam_velocity_, d * 72.0f, 0.2f);
        }
        prev_cam_ = cam;
        have_prev_cam_ = true;
    }
    const Vec3 cam_ahead = cam + cam_velocity_ * 1.5f;
    const bool prefetch = memory_pressure_ == 0 && glm::length(cam_velocity_) > 0.5f;

    // Rooms the player can see into: the region they stand in and everything
    // within two links (doorways can line up through a room). Off when the
    // player is outside every region.
    bool rooms_active = false;
    if (perf_.room_culling && can_cull && !regions_.empty()) {
        const int here = region_of(cam - Vec3(0.0f, 1.2f, 0.0f));
        if (here >= 0) {
            rooms_active = true;
            std::fill(room_visible_.begin(), room_visible_.end(), 0);
            std::vector<int> frontier{here};
            room_visible_[static_cast<size_t>(here)] = 1;
            for (int hop = 0; hop < 2; ++hop) {
                std::vector<int> next;
                for (int r : frontier) {
                    for (int n : region_links_[static_cast<size_t>(r)]) {
                        if (!room_visible_[static_cast<size_t>(n)]) {
                            room_visible_[static_cast<size_t>(n)] = 1;
                            next.push_back(n);
                        }
                    }
                }
                frontier.swap(next);
            }
        }
    }

    player_draw_names_.clear();
    // Notes, whichever path below drew this entity (they all ``continue``),
    // a draw sitting at the player's hands or feet that is not theirs.
    struct PlayerDrawWatch {
        Renderer& r;
        World& w;
        std::uint32_t e;
        Vec3 pos;
        const std::vector<DrawItem>& out;
        std::size_t before;
        ~PlayerDrawWatch() {
            if (out.size() > before) r.note_player_draw(w, e, pos);
        }
    };

    auto view = world.registry().view<const CTransform>();
    for (auto entity : view) {
        const auto& ct = view.get<const CTransform>(entity).t;
        ++stats_.entities_considered;
        PlayerDrawWatch watch{*this, world, static_cast<std::uint32_t>(entity), ct.position,
                              out, out.size()};

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
        // by the mesh, primitive and placeholder paths below.
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

        // Static geometry: drawn by its merged batch, or hidden with the
        // closed room it stands in.
        if (const auto* sb = world.registry().try_get<CStaticBatch>(entity)) {
            if (sb->batch >= 0 && static_cast<size_t>(sb->batch) < batches_.size() &&
                batches_[static_cast<size_t>(sb->batch)].valid) {
                continue;
            }
            if (rooms_active && sb->room >= 0 &&
                !room_visible_[static_cast<size_t>(sb->room)]) {
                ++stats_.culled_room;
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
                // A rigged mesh carries its model frame in its root joints;
                // a static one (an unrigged character) gets it here.
                Mat4 model = entity_model;
                if (cm.has_model_frame && cm.unit_scale > 1e-8f &&
                    !world.registry().all_of<CSkeleton>(entity)) {
                    const float u = cm.unit_scale;
                    model = model *
                            glm::translate(Mat4(1.0f),
                                           Vec3(0.0f,
                                                -(cm.model_sole_y +
                                                  0.5f * cm.model_height) * u,
                                                0.0f)) *
                            glm::rotate(Mat4(1.0f), cm.model_yaw_offset_rad,
                                        Vec3(0.0f, 1.0f, 0.0f));
                }
                const Vec3 center{model[3][0], model[3][1], model[3][2]};
                const float distance = can_cull ? glm::length(center - cam) : 0.0f;

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
                if (!first_person && can_cull &&
                    distance - radius > cull_distance_m_) {
                    ++stats_.culled_distance;
                    continue;
                }
                // Frustum cull against both eyes.
                if (!first_person && can_cull &&
                    !frustum.intersects_sphere(center, radius)) {
                    ++stats_.culled_frustum;
                    continue;
                }
                // Rendering-only cull for objects too small to see. Never
                // characters, highlighted (interactive) objects or the
                // player's own body; physics and gameplay are untouched.
                const CSkeleton* skel_probe = world.registry().try_get<CSkeleton>(entity);
                if (!first_person && can_cull && skel_probe == nullptr && highlight <= 0.0f &&
                    perf_.small_object_cull_px > 0.0f &&
                    projected_px(2.0f * radius, distance) < perf_.small_object_cull_px) {
                    ++stats_.culled_small;
                    continue;
                }

                // --- Pick a level of detail -------------------------------
                // By projected error: the coarsest level whose measured
                // simplification error stays under lod_screen_error_px on
                // screen (relaxed by the governor's lod_bias). Falls back to
                // the scene's switch distances when errors are unknown.
                const auto& chain = cm.lod_paths;
                int desired = 0;
                const bool by_error = cm.lod_errors_rel.size() == chain.size() && chain.size() > 1;
                if (by_error && can_cull && !first_person) {
                    const float extent = 2.0f * radius;
                    const float limit =
                        std::max(0.05f, perf_.lod_screen_error_px) * (1.0f + std::max(0.0f, lod_bias_));
                    float err_max = 0.0f;
                    std::array<float, 8> px{};
                    const size_t n = std::min<size_t>(chain.size(), px.size());
                    for (size_t i = 0; i < n; ++i) {
                        err_max = std::max(err_max, cm.lod_errors_rel[i]);
                        px[i] = projected_px(err_max * extent, distance);
                        if (px[i] <= limit) desired = static_cast<int>(i);
                    }
                    const auto prev_it = entity_lod_.find(static_cast<std::uint32_t>(entity));
                    if (prev_it != entity_lod_.end()) {
                        const int prev = prev_it->second;
                        if (desired > prev) {
                            desired = prev + 1;  // one step coarser per frame
                        } else if (desired < prev && static_cast<size_t>(prev) < n) {
                            // Finer only once the current level is clearly too
                            // coarse, so a mesh on the boundary does not pop.
                            desired = px[static_cast<size_t>(prev)] >
                                              limit * (1.0f + perf_.lod_hysteresis)
                                          ? prev - 1
                                          : prev;
                        }
                    }
                    desired = std::max(0, std::min(desired, static_cast<int>(chain.size()) - 1));
                    if (prefetch && skel_probe == nullptr) {
                        const float dist_ahead = glm::length(center - cam_ahead);
                        int ahead = 0;
                        float em = 0.0f;
                        for (size_t i = 0; i < n; ++i) {
                            em = std::max(em, cm.lod_errors_rel[i]);
                            if (projected_px(em * extent, dist_ahead) <= limit) {
                                ahead = static_cast<int>(i);
                            }
                        }
                        if (ahead < desired) {
                            request_stream(chain[static_cast<size_t>(ahead)],
                                           cm.target_scale_meters, cm.unit_scale,
                                           dist_ahead + 5.0f);
                        }
                    }
                } else if (chain.size() > 1 && can_cull && !first_person) {
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
                // Critical memory: visible meshes fall back to their coarsest
                // (always-resident) level -- a quality drop, never a crash.
                if (memory_pressure_ >= 3 && !first_person && chain.size() > 1) {
                    desired = static_cast<int>(chain.size()) - 1;
                }

                // --- Resolve to a resident mesh ---------------------------
                // Never block on I/O here. Request the LOD we want and, until
                // it arrives, draw any coarser level that is already resident.
                const std::string& wanted =
                    chain.empty() ? cm.glb_path
                                  : chain[static_cast<size_t>(desired)];
                // A rigged entity must never go through the streaming worker:
                // that path parses GLBs as static geometry and would cache a
                // static slot under the skinned mesh's own path, after which
                // the entity draws with the wrong pipeline forever. The scene
                // loader uploads skinned meshes up front instead.
                const CSkeleton* skel =
                    world.registry().try_get<CSkeleton>(entity);
                // Skinned meshes are uploaded by target scale at scene load;
                // static ones stream with the asset's single unit scale when
                // the pipeline provided one.
                const float unit =
                    (skel && !cm.has_model_frame) ? 0.0f : cm.unit_scale;
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
                    // GLB meshes carry their own base colour (texture x
                    // factor x COLOR_0), so the tint is white unless a
                    // category cue or a highlight asks otherwise -- a grey
                    // default tint would darken every texture.
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
            if (can_cull && !visuals.is_ground) {
                const Vec3 c{it.model[3][0], it.model[3][1], it.model[3][2]};
                const float r = 0.5f * glm::length(prim.size) *
                                max_axis_scale(entity_model);
                if (glm::length(c - cam) - r > cull_distance_m_) {
                    ++stats_.culled_distance;
                    continue;
                }
                if (!frustum.intersects_sphere(c, r)) {
                    ++stats_.culled_frustum;
                    continue;
                }
                if (highlight <= 0.0f && perf_.small_object_cull_px > 0.0f &&
                    projected_px(2.0f * r, glm::length(c - cam)) < perf_.small_object_cull_px) {
                    ++stats_.culled_small;
                    continue;
                }
            }
            const int slot = it.mesh_slot >= 0 ? it.mesh_slot : prim_slot_cube_;
            if (slot >= 0) {
                stats_.visible_triangles +=
                    gltf_meshes_[static_cast<size_t>(slot)].triangle_count;
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

        // Nothing to draw. Only a tag that ASKS for a placeholder gets one;
        // the default grey cube drew every logic host and collider proxy
        // (the player's finger colliders as boxes over their hands).
        if (!render_tags::draws_meshless(visuals)) {
            ++stats_.meshless_hidden;
            if (reported_meshless_.insert(static_cast<std::uint32_t>(entity)).second) {
                const std::string* id = world.id_of(entity);
                PTV_INFO("render: '{}' has nothing to draw (no mesh, primitive or "
                         "placeholder tag); not drawn",
                         id != nullptr ? *id : std::to_string(static_cast<std::uint32_t>(entity)));
            }
            continue;
        }
        if (!visuals.is_ground) {
            ++stats_.placeholders;
            if (reported_meshless_.insert(static_cast<std::uint32_t>(entity)).second) {
                const std::string* id = world.id_of(entity);
                PTV_WARN("render: '{}' has no mesh; drawn as a placeholder box",
                         id != nullptr ? *id : std::to_string(static_cast<std::uint32_t>(entity)));
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
        if (prim_slot_cube_ >= 0) {
            stats_.visible_triangles +=
                gltf_meshes_[static_cast<size_t>(prim_slot_cube_)].triangle_count;
        }
    }

    // Merged static geometry: culled as a whole (room, distance, frustum).
    for (const StaticBatch& b : batches_) {
        if (!b.valid || b.slot < 0) continue;
        if (rooms_active && b.room >= 0 && !room_visible_[static_cast<size_t>(b.room)]) {
            ++stats_.culled_room;
            continue;
        }
        if (can_cull && b.material_id < 0.5f) {
            if (glm::length(b.center - cam) - b.radius > cull_distance_m_) {
                ++stats_.culled_distance;
                continue;
            }
            if (!frustum.intersects_sphere(b.center, b.radius)) {
                ++stats_.culled_frustum;
                continue;
            }
        }
        DrawItem it{};
        it.model = Mat4(1.0f);
        it.mesh_slot = b.slot;
        it.color = Vec4(1.0f);
        it.material_id = b.material_id;
        out.push_back(it);
        ++stats_.drawn;
        ++stats_.batches_drawn;
        stats_.visible_triangles += b.triangles;
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
    text_.shutdown();
    fx_.shutdown();
    for (auto& r : retired_meshes_) destroy_mesh_slot(r.mesh);
    retired_meshes_.clear();
    destroy_gltf_meshes();
    textures_.shutdown();
    for (auto& kv : framebuffers_) {
        if (kv.second) vkDestroyFramebuffer(vk_->device(), kv.second, nullptr);
    }
    framebuffers_.clear();
    for (auto& kv : color_views_) {
        if (kv.second) vkDestroyImageView(vk_->device(), kv.second, nullptr);
    }
    color_views_.clear();
    for (auto& kv : density_views_) {
        if (kv.second) vkDestroyImageView(vk_->device(), kv.second, nullptr);
    }
    density_views_.clear();
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
    target_bytes_ = 0;

    // Frame UBOs + descriptors, one set per frame in flight (VMA unmaps
    // persistently-mapped buffers on destroy).
    for (int s = 0; s < kMaxFramesInFlight; ++s) {
        select_frame_slot(s);
        if (frame_ubo_) vmaDestroyBuffer(vk_->allocator(), frame_ubo_, frame_ubo_alloc_);
        frame_ubo_ = VK_NULL_HANDLE;
        frame_ubo_alloc_ = nullptr;
        frame_ubo_mapped_ = nullptr;
        if (joint_ssbo_)
            vmaDestroyBuffer(vk_->allocator(), joint_ssbo_, joint_ssbo_alloc_);
        joint_ssbo_ = VK_NULL_HANDLE;
        joint_ssbo_alloc_ = nullptr;
        joint_ssbo_mapped_ = nullptr;
        joint_ssbo_capacity_ = 0;
        if (instance_ssbo_)
            vmaDestroyBuffer(vk_->allocator(), instance_ssbo_, instance_ssbo_alloc_);
        instance_ssbo_ = VK_NULL_HANDLE;
        instance_ssbo_alloc_ = nullptr;
        instance_ssbo_mapped_ = nullptr;
        instance_capacity_ = 0;
        if (frame_fence_) vkDestroyFence(vk_->device(), frame_fence_, nullptr);
        frame_fence_ = VK_NULL_HANDLE;
        cmd_ = VK_NULL_HANDLE;
        frame_set_ = VK_NULL_HANDLE;
    }
    select_frame_slot(0);
    joint_palettes_.clear();
    if (desc_pool_) vkDestroyDescriptorPool(vk_->device(), desc_pool_, nullptr);
    if (frame_set_layout_)
        vkDestroyDescriptorSetLayout(vk_->device(), frame_set_layout_, nullptr);
    desc_pool_ = VK_NULL_HANDLE;
    frame_set_layout_ = VK_NULL_HANDLE;
    frame_set_ = VK_NULL_HANDLE;

    if (vignette_pipeline_)
        vkDestroyPipeline(vk_->device(), vignette_pipeline_, nullptr);
    if (vignette_fs_) vkDestroyShaderModule(vk_->device(), vignette_fs_, nullptr);
    vignette_pipeline_ = VK_NULL_HANDLE;
    vignette_fs_ = VK_NULL_HANDLE;
    if (sky_pipeline_) vkDestroyPipeline(vk_->device(), sky_pipeline_, nullptr);
    if (sky_vs_) vkDestroyShaderModule(vk_->device(), sky_vs_, nullptr);
    if (sky_fs_) vkDestroyShaderModule(vk_->device(), sky_fs_, nullptr);
    sky_pipeline_ = VK_NULL_HANDLE;
    sky_vs_ = sky_fs_ = VK_NULL_HANDLE;

    for (auto& p : pipelines_) {
        if (p) vkDestroyPipeline(vk_->device(), p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (pipeline_layout_)
        vkDestroyPipelineLayout(vk_->device(), pipeline_layout_, nullptr);
    if (vs_) vkDestroyShaderModule(vk_->device(), vs_, nullptr);
    if (fs_) vkDestroyShaderModule(vk_->device(), fs_, nullptr);
    if (skinned_vs_) vkDestroyShaderModule(vk_->device(), skinned_vs_, nullptr);
    if (render_pass_) vkDestroyRenderPass(vk_->device(), render_pass_, nullptr);
    pipeline_layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = skinned_vs_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;

    if (timestamp_pool_) vkDestroyQueryPool(vk_->device(), timestamp_pool_, nullptr);
    timestamp_pool_ = VK_NULL_HANDLE;
    if (cmd_pool_) vkDestroyCommandPool(vk_->device(), cmd_pool_, nullptr);
    frame_fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

}  // namespace ptv
