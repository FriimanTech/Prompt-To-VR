#pragma once

// GPU half of world-space text: the SDF atlas texture, a per-frame vertex
// buffer and an alpha-blended pipeline drawn after the geometry pass of each
// eye. Owned by Renderer; see TextAtlas for the layout.

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <string>
#include <vector>

#include "ptv/math/Math.h"
#include "ptv/render/TextAtlas.h"

namespace ptv {

class VulkanContext;

/** One string to draw this frame. */
struct TextItem {
    std::string text;
    Vec3 position{0.0f};
    /** Text plane: +X reading direction, +Y up, faces +Z. */
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float height_m{0.05f};
    Vec4 color{1.0f};
    /** 0 left, 1 centre, 2 right. */
    int align{1};
};

class TextRenderer {
public:
    bool initialize(VulkanContext& vk, VkRenderPass pass,
                    VkSampleCountFlagBits samples, VkCommandPool pool);
    void shutdown();
    bool ready() const { return pipeline_ != VK_NULL_HANDLE; }

    const TextAtlas& atlas() const { return atlas_; }

    /** Lay out and upload this frame's strings. Call once per frame before
     *  recording, after the previous frame's fence. */
    void prepare(const std::vector<TextItem>& items);
    /** Draw the prepared strings for one eye. */
    /** Both eyes' view-projections: a multiview pass picks one per view, a
     *  per-eye pass passes the same matrix twice. */
    void record(VkCommandBuffer cmd, const std::array<Mat4, 2>& view_proj);
    /** Select the vertex buffer of one frame in flight (call before prepare). */
    void set_frame_slot(int slot);

private:
    struct Vertex {
        float pos[3];
        float uv[2];
        float color[4];
    };
    bool upload_atlas(VkCommandPool pool);
    bool create_pipeline(VkRenderPass pass, VkSampleCountFlagBits samples);
    bool ensure_capacity(std::size_t vertices);

    VulkanContext* vk_{nullptr};
    TextAtlas atlas_{};
    VkImage image_{VK_NULL_HANDLE};
    VmaAllocation image_alloc_{nullptr};
    VkImageView view_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkDescriptorPool pool_{VK_NULL_HANDLE};
    VkDescriptorSet set_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkShaderModule vs_{VK_NULL_HANDLE};
    VkShaderModule fs_{VK_NULL_HANDLE};
    VkBuffer vb_{VK_NULL_HANDLE};
    VmaAllocation vb_alloc_{nullptr};
    void* vb_mapped_{nullptr};
    std::size_t vb_capacity_{0};  // vertices
    std::uint32_t vertex_count_{0};
    static constexpr int kSlots = 2;
    struct SlotBuffer {
        VkBuffer vb{VK_NULL_HANDLE};
        VmaAllocation alloc{nullptr};
        void* mapped{nullptr};
        std::size_t capacity{0};
    };
    std::array<SlotBuffer, kSlots> slots_{};
    int slot_{0};
    std::vector<Vertex> scratch_;
    std::vector<TextQuad> quads_;
};

}  // namespace ptv
