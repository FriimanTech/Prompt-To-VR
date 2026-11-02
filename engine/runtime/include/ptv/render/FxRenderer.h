#pragma once

// GPU half of gameplay effects: muzzle flashes, tracers, energy bolts,
// beams, sparks, impact puffs and bullet-hole decals. Every effect is a quad
// built on the CPU each frame (``ptv::EffectsService`` owns the particles
// and their lifetimes) and drawn with two pipelines after the geometry pass:
// alpha-blended first (decals, smoke), then additive (everything that glows).
//
// No textures: the fragment shader draws each shape procedurally from the
// quad's UV, so effects cost no asset, no descriptor set and no sampler, and
// a Quest scene pays only for the pixels a flash actually covers. Quad count
// is capped by the scene's performance block (``max_fx_quads``).

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv {

class VulkanContext;

enum class FxShape : int {
    /** Soft round glow (flash core, energy impact, light-ish puff). */
    Glow = 0,
    /** Streak along its length (tracers, bolts, sparks), soft across. */
    Streak = 1,
    /** Bright core with a wide falloff across its width (beams). */
    Beam = 2,
    /** Star of rays (muzzle flash petals). */
    Star = 3,
    /** Dark disc with a soft rim, alpha-blended (bullet holes, scorch). */
    Decal = 4,
    /** Soft disc, alpha-blended (smoke, dust, blood mist). */
    Puff = 5,
};

/** One effect quad for this frame. */
struct FxQuad {
    FxShape shape{FxShape::Glow};
    /** Billboard centre, or the start of a stretched quad. */
    Vec3 a{0.0f};
    /** End of a stretched quad (Streak / Beam). Ignored for billboards. */
    Vec3 b{0.0f};
    /** Billboard diameter, or the width of a stretched quad, in metres. */
    float size{0.05f};
    Vec4 color{1.0f};
    /** Decals: the surface normal the quad lies on. */
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /** In-plane rotation for billboards / decals, radians. */
    float spin{0.0f};

    bool additive() const { return shape != FxShape::Decal && shape != FxShape::Puff; }
    bool stretched() const { return shape == FxShape::Streak || shape == FxShape::Beam; }
};

class FxRenderer {
public:
    bool initialize(VulkanContext& vk, VkRenderPass pass, VkSampleCountFlagBits samples);
    void shutdown();
    bool ready() const { return additive_ != VK_NULL_HANDLE; }

    /** Build and upload this frame's quads, facing ``camera`` (the midpoint
     *  of the eyes). At most ``max_quads``; the rest are dropped, oldest
     *  last-submitted first. Call once per frame after the previous fence. */
    void prepare(const std::vector<FxQuad>& quads, const Vec3& camera,
                 std::size_t max_quads);
    /** Draw the prepared quads for one eye. */
    /** Both eyes' view-projections (see TextRenderer::record). */
    void record(VkCommandBuffer cmd, const std::array<Mat4, 2>& view_proj);
    std::uint32_t quad_count() const { return (alpha_vertices_ + add_vertices_) / 6; }
    /** Select the vertex buffer of one frame in flight (call before prepare). */
    void set_frame_slot(int slot);

private:
    static constexpr int kSlots = 2;
    struct SlotBuffer {
        VkBuffer vb{VK_NULL_HANDLE};
        VmaAllocation alloc{nullptr};
        void* mapped{nullptr};
        std::size_t capacity{0};
    };
    std::array<SlotBuffer, kSlots> slots_{};
    int slot_{0};

    struct Vertex {
        float pos[3];
        float uv[2];
        float color[4];
        float shape;
    };
    bool create_pipelines(VkRenderPass pass, VkSampleCountFlagBits samples);
    VkPipeline make_pipeline(VkRenderPass pass, VkSampleCountFlagBits samples, bool additive);
    bool ensure_capacity(std::size_t vertices);
    void emit(const FxQuad& q, const Vec3& camera, std::vector<Vertex>& out) const;

    VulkanContext* vk_{nullptr};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkPipeline additive_{VK_NULL_HANDLE};
    VkPipeline alpha_{VK_NULL_HANDLE};
    VkShaderModule vs_{VK_NULL_HANDLE};
    VkShaderModule fs_{VK_NULL_HANDLE};
    VkBuffer vb_{VK_NULL_HANDLE};
    VmaAllocation vb_alloc_{nullptr};
    void* vb_mapped_{nullptr};
    std::size_t vb_capacity_{0};
    std::uint32_t alpha_vertices_{0};
    std::uint32_t add_vertices_{0};
    std::vector<Vertex> scratch_alpha_;
    std::vector<Vertex> scratch_add_;
};

}  // namespace ptv
