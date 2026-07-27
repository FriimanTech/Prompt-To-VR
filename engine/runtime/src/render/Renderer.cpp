#include "ptv/render/Renderer.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/GltfMeshLoad.h"
#include "ptv/render/VulkanContext.h"

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
    if (!create_sky_pipeline()) return false;
    if (!create_cube_mesh()) return false;
    if (!create_primitive_meshes()) return false;
    if (!create_depth_image()) return false;
    if (!create_msaa_color_image()) return false;
    if (!create_command_resources()) return false;
    PTV_INFO("Renderer ready: {}x{} per eye, color fmt {}, MSAA {}x",
             eye_width_, eye_height_, static_cast<int>(color_format_),
             static_cast<int>(samples_));
    return true;
}

VkSampleCountFlagBits Renderer::pick_sample_count() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(vk_->physical_device(), &props);
    // Only levels supported for BOTH color and depth framebuffers are usable
    // for our resolve pass. Cap at 4x per the plan's quality budget.
    const VkSampleCountFlags counts =
        props.limits.framebufferColorSampleCounts &
        props.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
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

bool Renderer::create_descriptors() {
    // Set 0, binding 0: environment/lighting UBO (fragment stage).
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
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

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo pci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
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
    return vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &gpci,
                                     nullptr, &pipeline_) == VK_SUCCESS;
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

int Renderer::create_mesh_slot(const std::vector<gltf_mesh_load::LitVertex>& verts,
                               const std::vector<std::uint32_t>& indices) {
    if (!vk_ || verts.empty() || indices.empty()) return -1;
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
    const VkDeviceSize vb_size =
        static_cast<VkDeviceSize>(verts.size() * sizeof(verts[0]));
    const VkDeviceSize ib_size =
        static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));
    if (!make_vma_buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vb_size, gm.vb,
                         gm.vb_alloc)) {
        return -1;
    }
    upload_buffer(*vk_, gm.vb, gm.vb_alloc, verts.data(),
                  static_cast<size_t>(vb_size));
    if (!make_vma_buffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ib_size, gm.ib,
                         gm.ib_alloc)) {
        vmaDestroyBuffer(vk_->allocator(), gm.vb, gm.vb_alloc);
        return -1;
    }
    upload_buffer(*vk_, gm.ib, gm.ib_alloc, indices.data(),
                  static_cast<size_t>(ib_size));
    gm.index_count = static_cast<uint32_t>(indices.size());
    const int slot = static_cast<int>(gltf_meshes_.size());
    gltf_meshes_.push_back(gm);
    return slot;
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
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout_, 0, 1, &env_set_, 0, nullptr);

        Mat4 vp_mat = eye.projection * eye.view;
        for (const auto& it : items) {
            PushConstants pc{};
            pc.view_proj = vp_mat;
            pc.model = it.model;
            pc.color = it.color;
            pc.params = Vec4{it.material_id, 0.0f, 0.0f, 0.0f};
            vkCmdPushConstants(cmd_, pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);

            if (it.mesh_slot >= 0 &&
                static_cast<size_t>(it.mesh_slot) < gltf_meshes_.size()) {
                const GltfGpuMesh& gm = gltf_meshes_[static_cast<size_t>(
                    it.mesh_slot)];
                VkDeviceSize voff = 0;
                vkCmdBindVertexBuffers(cmd_, 0, 1, &gm.vb, &voff);
                vkCmdBindIndexBuffer(cmd_, gm.ib, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd_, gm.index_count, 1, 0, 0, 0);
            } else {
                VkDeviceSize voff = 0;
                vkCmdBindVertexBuffers(cmd_, 0, 1, &cube_vb_, &voff);
                vkCmdBindIndexBuffer(cmd_, cube_ib_, 0, VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cmd_, cube_index_count_, 1, 0, 0, 0);
            }
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
    // Built-in primitive meshes lived in gltf_meshes_; their slots are now
    // invalid.
    prim_slot_sphere_ = prim_slot_cylinder_ = prim_slot_plane_ =
        prim_slot_ramp_ = -1;
}

int Renderer::load_glb(const std::string& resolved_path,
                       float target_scale_meters) {
    return ensure_gltf_mesh(resolved_path, target_scale_meters);
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
    auto it = gltf_path_to_slot_.find(resolved_path);
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
    gltf_path_to_slot_[resolved_path] = slot;
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

void Renderer::build_items_from_world(World& world, std::vector<DrawItem>& out) {
    out.clear();
    auto view = world.registry().view<const CTransform>();
    for (auto entity : view) {
        const auto& ct = view.get<const CTransform>(entity).t;

        // --- Skip invisible / logic-only entities --------------------------------
        // Generated scenes often include dozens of ``mesh: null`` trigger /
        // tutorial volumes. Without this guard the renderer falls through to
        // the procedural cube path, littering the world with cyan "starter
        // room" debug cubes *on top of* the real Tripo environment. Template
        // meshes (construct prefabs parked at a far staging coordinate) are
        // also hidden so only runtime-spawned copies draw.
        if (world.registry().all_of<CTag>(entity)) {
            const auto& tags = world.registry().get<CTag>(entity).tags;
            auto has_tag = [&](const char* t) {
                for (const auto& s : tags) {
                    if (s == t) return true;
                }
                return false;
            };
            if (has_tag("template")) {
                continue;
            }
            const bool meshless =
                !world.registry().all_of<CMesh>(entity) ||
                world.registry().get<CMesh>(entity).glb_path.empty();
            if (meshless &&
                (has_tag("player_system") || has_tag("tutorial") ||
                 has_tag("checkpoint"))) {
                continue;
            }
        }

        if (world.registry().all_of<CMesh>(entity)) {
            const auto& cm = world.registry().get<CMesh>(entity);
            if (!cm.glb_path.empty()) {
                int slot =
                    ensure_gltf_mesh(cm.glb_path, cm.target_scale_meters);
                if (slot >= 0) {
                    DrawItem it{};
                    it.model = ct.matrix();
                    it.mesh_slot = slot;
                    it.material_id = 0.0f;
                    // GLB primitives carry their own per-vertex baseColor
                    // (see GltfMeshLoad). Use a near-white tint so the
                    // material color drives the look; we only nudge the
                    // tint for category cues and never crush the source.
                    it.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
                    if (world.registry().all_of<CTag>(entity)) {
                        const auto& tags = world.registry().get<CTag>(entity).tags;
                        for (const auto& tag : tags) {
                            if (tag == "environment" ||
                                tag == "hover_allowed") {
                                // Cool, very subtle so skybox / terrain
                                // baseColor still reads.
                                it.color = Vec4{0.96f, 0.98f, 1.05f, 1.0f};
                                break;
                            }
                            if (tag == "enemy") {
                                // Warm rim cue without overpowering the
                                // source palette.
                                it.color = Vec4{1.05f, 0.95f, 0.92f, 1.0f};
                                break;
                            }
                            if (tag == "grabbable") {
                                // Slight pop so prop grabs read in VR.
                                it.color = Vec4{1.05f, 1.02f, 0.98f, 1.0f};
                                break;
                            }
                        }
                    }
                    out.push_back(it);
                    continue;
                }
            }
        }

        // --- Procedural primitive from the world-generation stage ----------
        if (world.registry().all_of<CPrimitive>(entity)) {
            const auto& prim = world.registry().get<CPrimitive>(entity);
            bool is_ground = false;
            if (world.registry().all_of<CTag>(entity)) {
                for (const auto& s : world.registry().get<CTag>(entity).tags) {
                    if (s == "ground" || s == "teleport_target") {
                        is_ground = true;
                        break;
                    }
                }
            }
            DrawItem it{};
            Mat4 s = glm::scale(Mat4(1.0f),
                                Vec3(prim.size.x, prim.size.y, prim.size.z));
            it.model = ct.matrix() * s;
            it.color = Vec4{prim.color.x, prim.color.y, prim.color.z, 1.0f};
            it.material_id = is_ground ? 1.0f : 0.0f;
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
            out.push_back(it);
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
                it.model = ct.matrix();
                it.mesh_slot = slot;
                it.color = Vec4{1.0f, 1.0f, 1.0f, 1.0f};  // vertex colors drive look
                it.material_id = 0.0f;
                out.push_back(it);
                continue;
            }
        }

        DrawItem it;
        it.model = ct.matrix();
        it.mesh_slot = -1;

        // Size + color hints by tag.
        float xz_scale = 0.3f;
        float y_scale = 0.3f;
        Vec4 color{0.78f, 0.78f, 0.82f, 1.0f};
        float material_id = 0.0f;
        if (world.registry().all_of<CTag>(entity)) {
            const auto& tags = world.registry().get<CTag>(entity).tags;
            for (const auto& tag : tags) {
                if (tag == "ground" || tag == "teleport_target") {
                    // Push the floor well past the play area so the edge
                    // never shows in the headset. The fragment shader draws
                    // a fading grid inside the render-distance horizon.
                    xz_scale = 30.0f;
                    y_scale = 0.05f;
                    color = Vec4{0.22f, 0.24f, 0.27f, 1.0f};
                    material_id = 1.0f;
                } else if (tag == "grabbable") {
                    xz_scale = y_scale = 0.18f;
                    color = Vec4{0.95f, 0.55f, 0.22f, 1.0f};
                } else if (tag == "trigger") {
                    xz_scale = y_scale = 0.20f;
                    color = Vec4{0.30f, 0.75f, 0.98f, 1.0f};
                }
            }
        }
        // Bake a non-uniform scale into the model matrix without touching
        // the Transform's stored scale (which is uniform).
        Mat4 s = glm::scale(Mat4(1.0f), Vec3(xz_scale, y_scale, xz_scale));
        it.model = it.model * s;
        it.color = color;
        it.material_id = material_id;
        out.push_back(it);
    }
}

void Renderer::shutdown() {
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
    if (desc_pool_) vkDestroyDescriptorPool(vk_->device(), desc_pool_, nullptr);
    if (env_set_layout_)
        vkDestroyDescriptorSetLayout(vk_->device(), env_set_layout_, nullptr);
    desc_pool_ = VK_NULL_HANDLE;
    env_set_layout_ = VK_NULL_HANDLE;
    env_set_ = VK_NULL_HANDLE;

    if (sky_pipeline_) vkDestroyPipeline(vk_->device(), sky_pipeline_, nullptr);
    if (sky_pipeline_layout_)
        vkDestroyPipelineLayout(vk_->device(), sky_pipeline_layout_, nullptr);
    if (sky_vs_) vkDestroyShaderModule(vk_->device(), sky_vs_, nullptr);
    if (sky_fs_) vkDestroyShaderModule(vk_->device(), sky_fs_, nullptr);
    sky_pipeline_ = VK_NULL_HANDLE;
    sky_pipeline_layout_ = VK_NULL_HANDLE;
    sky_vs_ = sky_fs_ = VK_NULL_HANDLE;

    if (pipeline_) vkDestroyPipeline(vk_->device(), pipeline_, nullptr);
    if (pipeline_layout_)
        vkDestroyPipelineLayout(vk_->device(), pipeline_layout_, nullptr);
    if (vs_) vkDestroyShaderModule(vk_->device(), vs_, nullptr);
    if (fs_) vkDestroyShaderModule(vk_->device(), fs_, nullptr);
    if (render_pass_) vkDestroyRenderPass(vk_->device(), render_pass_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;

    if (frame_fence_) vkDestroyFence(vk_->device(), frame_fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(vk_->device(), cmd_pool_, nullptr);
    frame_fence_ = VK_NULL_HANDLE;
    cmd_pool_ = VK_NULL_HANDLE;
    cmd_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

}  // namespace ptv
