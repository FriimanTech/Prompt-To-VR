#include "ptv/render/Renderer.h"

#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"
#include "ptv/render/VulkanContext.h"

namespace ptv {

namespace {

#ifndef PTV_SHADER_DIR
#define PTV_SHADER_DIR "shaders"
#endif

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        PTV_ERROR("Could not open shader SPIR-V: {}", path);
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string s = oss.str();
    return {s.begin(), s.end()};
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

struct Vertex {
    float pos[3];
    float normal[3];
};

// A unit cube centered at the origin with side length 1. Each face gets a
// flat outward normal (6 faces x 4 vertices = 24 unique vertices), which
// makes lighting look correct without smoothing.
const std::array<Vertex, 24> kCubeVerts = {{
    // +X
    {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}},
    // -X
    {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}},
    {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}},
    {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}},
    {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}},
    // +Y (top)
    {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}},
    {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}},
    {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}},
    {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}},
    // -Y
    {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}},
    {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}},
    {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}},
    {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}},
    // +Z
    {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}},
    {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}},
    {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}},
    {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}},
    // -Z
    {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}},
    {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}},
    {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}},
    {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}},
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
    if (!create_render_pass()) return false;
    if (!create_pipeline()) return false;
    if (!create_cube_mesh()) return false;
    if (!create_depth_image()) return false;
    if (!create_command_resources()) return false;
    PTV_INFO("Renderer ready: {}x{} per eye, color fmt {}",
             eye_width_, eye_height_, static_cast<int>(color_format_));
    return true;
}

bool Renderer::create_render_pass() {
    VkAttachmentDescription color{};
    color.format = color_format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // The XR runtime expects the swapchain image to arrive in
    // COLOR_ATTACHMENT_OPTIMAL; it handles the present transition itself.
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format = depth_format_;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;

    std::array<VkAttachmentDescription, 2> atts{color, depth};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = static_cast<uint32_t>(atts.size());
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    return vkCreateRenderPass(vk_->device(), &ci, nullptr, &render_pass_) ==
           VK_SUCCESS;
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

    VkVertexInputAttributeDescription vattr[2]{};
    vattr[0].location = 0;
    vattr[0].binding = 0;
    vattr[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vattr[0].offset = offsetof(Vertex, pos);
    vattr[1].location = 1;
    vattr[1].binding = 0;
    vattr[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    vattr[1].offset = offsetof(Vertex, normal);

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = 2;
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
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

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
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
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
    VkImageView color_v = get_or_create_color_view(img);
    if (!color_v) return VK_NULL_HANDLE;
    std::array<VkImageView, 2> atts{color_v, depth_view_};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = render_pass_;
    fci.attachmentCount = static_cast<uint32_t>(atts.size());
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
                          const Vec3& sky_color) {
    // Wait for last frame to finish before re-recording the same cmdbuf.
    vkWaitForFences(vk_->device(), 1, &frame_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(vk_->device(), 1, &frame_fence_);

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);

    for (const auto& eye : eyes) {
        VkFramebuffer fb = get_or_create_framebuffer(eye.image);
        if (!fb) continue;

        std::array<VkClearValue, 2> clears{};
        clears[0].color = {{sky_color.x, sky_color.y, sky_color.z, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpi.renderPass = render_pass_;
        rpi.framebuffer = fb;
        rpi.renderArea.offset = {eye.offset_x, eye.offset_y};
        rpi.renderArea.extent = {eye.width, eye.height};
        rpi.clearValueCount = static_cast<uint32_t>(clears.size());
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

        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmd_, 0, 1, &cube_vb_, &voff);
        vkCmdBindIndexBuffer(cmd_, cube_ib_, 0, VK_INDEX_TYPE_UINT16);

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
            vkCmdDrawIndexed(cmd_, cube_index_count_, 1, 0, 0, 0);
        }
        vkCmdEndRenderPass(cmd_);
    }
    vkEndCommandBuffer(cmd_);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, frame_fence_);
}

void Renderer::build_items_from_world(World& world,
                                      std::vector<DrawItem>& out) const {
    out.clear();
    auto view = world.registry().view<const CTransform>();
    for (auto entity : view) {
        const auto& ct = view.get<const CTransform>(entity).t;
        DrawItem it;
        it.model = ct.matrix();

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

    if (cube_vb_) vmaDestroyBuffer(vk_->allocator(), cube_vb_, cube_vb_alloc_);
    if (cube_ib_) vmaDestroyBuffer(vk_->allocator(), cube_ib_, cube_ib_alloc_);
    cube_vb_ = cube_ib_ = VK_NULL_HANDLE;
    cube_vb_alloc_ = cube_ib_alloc_ = nullptr;

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
