#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <volk.h>
#include <vk_mem_alloc.h>

#include "ptv/platform/AssetSource.h"
#include "ptv/render/GltfMeshLoad.h"

namespace ptv {

class VulkanContext;

/** Where one base-colour image comes from. ``key`` identifies the image for
 *  sharing: the resolved KTX2 / uri path, or ``<glb>#<hash>`` for embedded
 *  bytes, so the same picture used by many meshes and LODs is uploaded once. */
struct TextureRequest {
    std::string key;
    /** Resolved KTX2 path (asset-source relative), preferred when ASTC works. */
    std::string ktx2_path;
    /** Resolved PNG/JPEG path, or empty for an embedded image. */
    std::string fallback_path;
    /** Encoded bytes of an embedded image (copied from the GLB). */
    std::vector<unsigned char> embedded;
    /** Hash of ``embedded``, used to look for a packaged ASTC twin. */
    std::string embedded_hash;
    /** Directory of the GLB, for the ``ptv_textures/<hash>.ktx2`` twin of an
     *  embedded image. */
    std::string glb_dir;
    /** For diagnostics: which asset / material wanted it. */
    std::string label;
};

/** A decoded image ready for upload: either GPU-compressed levels (ASTC from
 *  KTX2) or one RGBA8 level that the upload expands into a mip chain. */
struct TextureData {
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint32_t width{0};
    std::uint32_t height{0};
    struct Level {
        std::size_t offset{0};
        std::size_t size{0};
        std::uint32_t width{0};
        std::uint32_t height{0};
    };
    std::vector<Level> levels;
    std::vector<std::uint8_t> bytes;
    /** KTX2 levels straight from a memory-mapped file (Quest APK), used
     *  instead of ``bytes`` when set -- no heap copy of compressed data. */
    AssetView mapped;
    const std::uint8_t* data() const { return mapped.data ? mapped.data : bytes.data(); }
    bool generate_mips{false};
    /** "ktx2" / "png" / "jpg" / "embedded" -- telemetry and the load log. */
    std::string source;
};

/** Shared, reference-counted GPU textures for mesh materials.
 *
 * Descriptor set layout: set 1, binding 0 = combined image sampler. Each
 * texture owns one descriptor set (image + the sampler its material asked
 * for), so a draw binds exactly one set per material change.
 *
 * Decoding (file read + KTX2 parse or stb decode) is pure CPU and safe on the
 * streaming worker; ``upload`` runs on the render thread. Uploads go through
 * one persistent staging ring and wait for the copy before returning, so no
 * CPU copy outlives the call. */
class TextureCache {
public:
    struct Stats {
        std::uint32_t resident{0};
        std::uint64_t bytes{0};
        std::uint32_t ktx2{0};
        std::uint32_t decoded{0};      // PNG/JPEG decoded on device
        std::uint32_t missing{0};      // referenced but could not be loaded
        std::uint32_t uploads{0};
    };

    bool initialize(VulkanContext& vk, bool anisotropy);
    void shutdown();

    VkDescriptorSetLayout set_layout() const { return set_layout_; }
    VkDescriptorSet white_set() const { return white_set_; }
    VkDescriptorSet missing_set() const { return missing_set_; }

    /** ASTC LDR usable on this device (feature + format support). */
    bool astc_supported() const { return astc_.load(); }

    /** Thread-safe: true if ``key`` is resident or already decoded. */
    bool has(const std::string& key) const;

    /** Thread-safe CPU decode. ``max_dim`` caps a decoded PNG/JPEG (halved
     *  until it fits) so an oversized embedded image cannot blow memory. */
    bool decode(const TextureRequest& req, TextureData& out, std::string& err,
                std::uint32_t max_dim = 2048) const;

    /** Upload decoded data under ``key`` with a sampler; returns a texture id
     *  holding one reference, or -1. An existing key just gains a reference. */
    int upload(const std::string& key, TextureData&& data,
               const gltf_mesh_load::SamplerInfo& sampler);

    /** Existing texture for ``key`` + sampler (gains a reference), or -1. */
    int acquire(const std::string& key, const gltf_mesh_load::SamplerInfo& sampler);
    void release(int id);

    VkDescriptorSet descriptor(int id) const;
    std::uint64_t texture_bytes(int id) const;

    /** Free textures whose last reference went away ``frames`` ago or more. */
    void collect(std::uint64_t frame_index);
    void set_frame(std::uint64_t frame_index) { frame_index_ = frame_index; }

    const Stats& stats() const { return stats_; }
    void note_missing() { ++stats_.missing; }

private:
    struct Entry {
        std::string key;
        VkImage image{VK_NULL_HANDLE};
        VmaAllocation alloc{nullptr};
        VkImageView view{VK_NULL_HANDLE};
        VkDescriptorSet set{VK_NULL_HANDLE};
        VkDescriptorPool pool{VK_NULL_HANDLE};
        std::uint64_t bytes{0};
        int refs{0};
        std::uint64_t released_frame{0};
        bool live{false};
    };

    VkSampler sampler_for(const gltf_mesh_load::SamplerInfo& s, std::uint32_t mip_levels);
    bool create_image(TextureData& data, Entry& e, std::string& err);
    VkDescriptorSet allocate_set(VkImageView view, VkSampler sampler, VkDescriptorPool& pool);
    bool ensure_staging(std::size_t bytes);
    bool make_solid(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                    VkDescriptorSet& out_set, Entry& out_entry);
    void destroy_entry(Entry& e);
    static std::string sampler_key(const std::string& key,
                                   const gltf_mesh_load::SamplerInfo& s);

    VulkanContext* vk_{nullptr};
    std::atomic<bool> astc_{false};
    bool anisotropy_{false};
    bool blit_srgb_{false};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    std::vector<VkDescriptorPool> pools_;
    std::uint32_t pool_used_{0};
    VkCommandPool cmd_pool_{VK_NULL_HANDLE};
    VkFence fence_{VK_NULL_HANDLE};
    VkBuffer staging_{VK_NULL_HANDLE};
    VmaAllocation staging_alloc_{nullptr};
    void* staging_mapped_{nullptr};
    std::size_t staging_size_{0};

    std::unordered_map<std::string, VkSampler> samplers_;
    std::vector<Entry> entries_;
    std::vector<int> free_ids_;
    std::unordered_map<std::string, int> by_key_;  // key + sampler -> id
    mutable std::mutex key_mutex_;
    std::unordered_set<std::string> known_keys_;   // image keys, thread-safe view

    Entry white_{};
    Entry missing_{};
    VkDescriptorSet white_set_{VK_NULL_HANDLE};
    VkDescriptorSet missing_set_{VK_NULL_HANDLE};
    Stats stats_{};
    std::uint64_t frame_index_{0};
};

}  // namespace ptv
