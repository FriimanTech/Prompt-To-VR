#include "ptv/render/FxRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/VulkanContext.h"

namespace ptv {
namespace {

VkShaderModule load_shader(VkDevice dev, const std::string& path) {
    auto bytes = asset_source().read_bytes(path);
    if (!bytes || bytes->empty()) {
        PTV_WARN("fx: shader not found: {}", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = bytes->size();
    sci.pCode = reinterpret_cast<const uint32_t*>(bytes->data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &sci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

Vec3 any_perp(const Vec3& v) {
    const Vec3 a = std::fabs(v.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    return glm::normalize(glm::cross(v, a));
}

}  // namespace

bool FxRenderer::initialize(VulkanContext& vk, VkRenderPass pass,
                            VkSampleCountFlagBits samples) {
    vk_ = &vk;
    if (!create_pipelines(pass, samples)) {
        PTV_WARN("fx: pipeline creation failed; gameplay effects disabled");
        return false;
    }
    return true;
}

VkPipeline FxRenderer::make_pipeline(VkRenderPass pass, VkSampleCountFlagBits samples,
                                     bool additive) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbd{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
        {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, shape)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // Decals sit ON a surface: pull them toward the camera a little so they
    // do not z-fight the wall they are on.
    if (!additive) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = -1.0f;
        rs.depthBiasSlopeFactor = -1.5f;
    }
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    // Tested against the world, never written: effects must not hide each
    // other or the geometry behind them.
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    if (additive) {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    } else {
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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
    gpci.layout = layout_;
    gpci.renderPass = pass;
    gpci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk_->device(), VK_NULL_HANDLE, 1, &gpci, nullptr, &p) !=
        VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return p;
}

bool FxRenderer::create_pipelines(VkRenderPass pass, VkSampleCountFlagBits samples) {
    VkDevice dev = vk_->device();
    const std::string base = std::string(PTV_SHADER_DIR);
    vs_ = load_shader(dev, base + "/fx.vert.spv");
    fs_ = load_shader(dev, base + "/fx.frag.spv");
    if (!vs_ || !fs_) return false;
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout_) != VK_SUCCESS) return false;
    alpha_ = make_pipeline(pass, samples, false);
    additive_ = make_pipeline(pass, samples, true);
    return alpha_ != VK_NULL_HANDLE && additive_ != VK_NULL_HANDLE;
}

bool FxRenderer::ensure_capacity(std::size_t vertices) {
    if (vertices <= vb_capacity_ && vb_) return true;
    std::size_t cap = std::max<std::size_t>(vb_capacity_, 6 * 256);
    while (cap < vertices) cap *= 2;
    if (vb_) vmaDestroyBuffer(vk_->allocator(), vb_, vb_alloc_);
    vb_ = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = cap * sizeof(Vertex);
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &vb_, &vb_alloc_, &info) != VK_SUCCESS) {
        vb_capacity_ = 0;
        return false;
    }
    vb_mapped_ = info.pMappedData;
    vb_capacity_ = cap;
    return true;
}

void FxRenderer::emit(const FxQuad& q, const Vec3& camera, std::vector<Vertex>& out) const {
    Vec3 c[4];
    if (q.stretched()) {
        Vec3 axis = q.b - q.a;
        const float len = glm::length(axis);
        if (len < 1e-5f) return;
        axis /= len;
        const Vec3 mid = 0.5f * (q.a + q.b);
        Vec3 side = glm::cross(axis, camera - mid);
        if (glm::length(side) < 1e-6f) side = any_perp(axis);
        side = glm::normalize(side) * (0.5f * q.size);
        c[0] = q.a - side;
        c[1] = q.a + side;
        c[2] = q.b + side;
        c[3] = q.b - side;
    } else {
        Vec3 n = q.shape == FxShape::Decal ? glm::normalize(q.normal)
                                           : glm::normalize(camera - q.a);
        if (!std::isfinite(n.x)) n = Vec3(0, 1, 0);
        Vec3 u = any_perp(n);
        Vec3 v = glm::cross(n, u);
        const float cs = std::cos(q.spin), sn = std::sin(q.spin);
        const Vec3 ru = (u * cs + v * sn) * (0.5f * q.size);
        const Vec3 rv = (v * cs - u * sn) * (0.5f * q.size);
        // Decals hover 2 mm off their surface (plus depth bias).
        const Vec3 p = q.shape == FxShape::Decal ? q.a + n * 0.002f : q.a;
        c[0] = p - ru - rv;
        c[1] = p + ru - rv;
        c[2] = p + ru + rv;
        c[3] = p - ru + rv;
    }
    const float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    const float shape = static_cast<float>(static_cast<int>(q.shape));
    Vertex v[4]{};
    for (int k = 0; k < 4; ++k) {
        v[k] = Vertex{{c[k].x, c[k].y, c[k].z}, {uv[k][0], uv[k][1]},
                      {q.color.r, q.color.g, q.color.b, q.color.a}, shape};
    }
    for (int k : {0, 1, 2, 0, 2, 3}) out.push_back(v[k]);
}

void FxRenderer::prepare(const std::vector<FxQuad>& quads, const Vec3& camera,
                         std::size_t max_quads) {
    alpha_vertices_ = add_vertices_ = 0;
    if (!ready() || quads.empty()) return;
    scratch_alpha_.clear();
    scratch_add_.clear();
    const std::size_t n = std::min(quads.size(), max_quads);
    for (std::size_t i = 0; i < n; ++i) {
        const FxQuad& q = quads[i];
        emit(q, camera, q.additive() ? scratch_add_ : scratch_alpha_);
    }
    const std::size_t total = scratch_alpha_.size() + scratch_add_.size();
    if (total == 0 || !ensure_capacity(total)) return;
    auto* dst = static_cast<Vertex*>(vb_mapped_);
    std::memcpy(dst, scratch_alpha_.data(), scratch_alpha_.size() * sizeof(Vertex));
    std::memcpy(dst + scratch_alpha_.size(), scratch_add_.data(),
                scratch_add_.size() * sizeof(Vertex));
    vmaFlushAllocation(vk_->allocator(), vb_alloc_, 0, VK_WHOLE_SIZE);
    alpha_vertices_ = static_cast<std::uint32_t>(scratch_alpha_.size());
    add_vertices_ = static_cast<std::uint32_t>(scratch_add_.size());
}

void FxRenderer::record(VkCommandBuffer cmd, const Mat4& view_proj) {
    if (!ready() || (alpha_vertices_ + add_vertices_) == 0) return;
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb_, &off);
    if (alpha_vertices_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, alpha_);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                           &view_proj);
        vkCmdDraw(cmd, alpha_vertices_, 1, 0, 0);
    }
    if (add_vertices_ > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, additive_);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4),
                           &view_proj);
        vkCmdDraw(cmd, add_vertices_, 1, alpha_vertices_, 0);
    }
}

void FxRenderer::shutdown() {
    if (!vk_) return;
    VkDevice dev = vk_->device();
    if (additive_) vkDestroyPipeline(dev, additive_, nullptr);
    if (alpha_) vkDestroyPipeline(dev, alpha_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (vs_) vkDestroyShaderModule(dev, vs_, nullptr);
    if (fs_) vkDestroyShaderModule(dev, fs_, nullptr);
    if (vb_) vmaDestroyBuffer(vk_->allocator(), vb_, vb_alloc_);
    additive_ = alpha_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    vb_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

}  // namespace ptv
