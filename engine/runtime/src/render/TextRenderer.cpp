#include "ptv/render/TextRenderer.h"

#include <algorithm>
#include <cstring>

#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/VulkanContext.h"

namespace ptv {
namespace {

VkShaderModule load_shader(VkDevice dev, const std::string& path) {
    auto bytes = asset_source().read_bytes(path);
    if (!bytes || bytes->empty()) {
        PTV_WARN("text: shader not found: {}", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = bytes->size();
    sci.pCode = reinterpret_cast<const uint32_t*>(bytes->data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &sci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

}  // namespace

bool TextRenderer::initialize(VulkanContext& vk, VkRenderPass pass,
                              VkSampleCountFlagBits samples, VkCommandPool pool) {
    vk_ = &vk;
    std::string err;
    if (!atlas_.build_default(err)) {
        PTV_WARN("text: {}; world-space text disabled", err);
        return false;
    }
    if (!upload_atlas(pool)) {
        PTV_WARN("text: atlas upload failed; world-space text disabled");
        return false;
    }
    if (!create_pipeline(pass, samples)) {
        PTV_WARN("text: pipeline creation failed; world-space text disabled");
        return false;
    }
    return true;
}

bool TextRenderer::upload_atlas(VkCommandPool pool) {
    VkDevice dev = vk_->device();
    const VkDeviceSize size = static_cast<VkDeviceSize>(atlas_.pixels().size());

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8_UNORM;
    ici.extent = {static_cast<uint32_t>(atlas_.width()),
                  static_cast<uint32_t>(atlas_.height()), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vk_->allocator(), &ici, &aci, &image_, &image_alloc_, nullptr) !=
        VK_SUCCESS) {
        return false;
    }

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = nullptr;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo sci{};
    sci.usage = VMA_MEMORY_USAGE_AUTO;
    sci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vk_->allocator(), &bci, &sci, &staging, &staging_alloc, &info) !=
        VK_SUCCESS) {
        return false;
    }
    std::memcpy(info.pMappedData, atlas_.pixels().data(), static_cast<size_t>(size));
    vmaFlushAllocation(vk_->allocator(), staging_alloc, 0, VK_WHOLE_SIZE);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
        vmaDestroyBuffer(vk_->allocator(), staging, staging_alloc);
        return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image_;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_dst);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = ici.extent;
    vkCmdCopyBufferToImage(cmd, staging, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);
    VkImageMemoryBarrier to_read = to_dst;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_read);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(dev, &fci, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
    vmaDestroyBuffer(vk_->allocator(), staging, staging_alloc);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &view_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo smp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smp.magFilter = VK_FILTER_LINEAR;
    smp.minFilter = VK_FILTER_LINEAR;
    smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smp.maxLod = 0.0f;
    if (vkCreateSampler(dev, &smp, nullptr, &sampler_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set_layout_) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &pool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo dsi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsi.descriptorPool = pool_;
    dsi.descriptorSetCount = 1;
    dsi.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(dev, &dsi, &set_) != VK_SUCCESS) return false;
    VkDescriptorImageInfo dii{sampler_, view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    return true;
}

bool TextRenderer::create_pipeline(VkRenderPass pass, VkSampleCountFlagBits samples) {
    VkDevice dev = vk_->device();
    const std::string base = std::string(PTV_SHADER_DIR);
    vs_ = load_shader(dev, base + "/text.vert.spv");
    fs_ = load_shader(dev, base + "/text.frag.spv");
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

    VkVertexInputBindingDescription vbd{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3]{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbd;
    vi.vertexAttributeDescriptionCount = 3;
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
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    // Tested against the world (a wall in front hides a sign) but never
    // written: overlapping glyph quads must not clip each other.
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
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
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &layout_) != VK_SUCCESS) return false;

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
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_) ==
           VK_SUCCESS;
}

bool TextRenderer::ensure_capacity(std::size_t vertices) {
    if (vertices <= vb_capacity_ && vb_) return true;
    std::size_t cap = std::max<std::size_t>(vb_capacity_, 6 * 512);
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

void TextRenderer::prepare(const std::vector<TextItem>& items) {
    vertex_count_ = 0;
    if (!ready() || items.empty()) return;
    scratch_.clear();
    for (const auto& it : items) {
        if (it.text.empty() || !(it.height_m > 0.0f)) continue;
        quads_.clear();
        atlas_.layout(it.text, it.height_m, it.align, quads_);
        const Mat3 r = glm::mat3_cast(glm::normalize(it.rotation));
        for (const auto& q : quads_) {
            const Vec2 corner[4] = {{q.p0.x, q.p0.y}, {q.p1.x, q.p0.y},
                                    {q.p1.x, q.p1.y}, {q.p0.x, q.p1.y}};
            const Vec2 uv[4] = {{q.uv0.x, q.uv0.y}, {q.uv1.x, q.uv0.y},
                                {q.uv1.x, q.uv1.y}, {q.uv0.x, q.uv1.y}};
            Vertex v[4]{};
            for (int k = 0; k < 4; ++k) {
                const Vec3 p = it.position + r * Vec3(corner[k].x, corner[k].y, 0.0f);
                v[k] = Vertex{{p.x, p.y, p.z}, {uv[k].x, uv[k].y},
                              {it.color.r, it.color.g, it.color.b, it.color.a}};
            }
            for (int k : {0, 1, 2, 0, 2, 3}) scratch_.push_back(v[k]);
        }
    }
    if (scratch_.empty() || !ensure_capacity(scratch_.size())) return;
    std::memcpy(vb_mapped_, scratch_.data(), scratch_.size() * sizeof(Vertex));
    vmaFlushAllocation(vk_->allocator(), vb_alloc_, 0, VK_WHOLE_SIZE);
    vertex_count_ = static_cast<std::uint32_t>(scratch_.size());
}

void TextRenderer::record(VkCommandBuffer cmd, const Mat4& view_proj) {
    if (!ready() || vertex_count_ == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &view_proj);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb_, &off);
    vkCmdDraw(cmd, vertex_count_, 1, 0, 0);
}

void TextRenderer::shutdown() {
    if (!vk_) return;
    VkDevice dev = vk_->device();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (vs_) vkDestroyShaderModule(dev, vs_, nullptr);
    if (fs_) vkDestroyShaderModule(dev, fs_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    if (view_) vkDestroyImageView(dev, view_, nullptr);
    if (image_) vmaDestroyImage(vk_->allocator(), image_, image_alloc_);
    if (vb_) vmaDestroyBuffer(vk_->allocator(), vb_, vb_alloc_);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vs_ = fs_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    vb_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

}  // namespace ptv
