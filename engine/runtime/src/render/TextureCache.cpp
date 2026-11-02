#include "ptv/render/TextureCache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/Ktx2.h"
#include "ptv/render/VulkanContext.h"

// stb is header-only; STB_IMAGE_STATIC keeps every symbol internal to this
// translation unit, so tools that link ptv_runtime and define their own
// STB_IMAGE_IMPLEMENTATION (ptv_glb_views) do not collide with it.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

namespace ptv {
namespace {

constexpr std::size_t kStagingBytes = 32u * 1024u * 1024u;
constexpr std::uint32_t kSetsPerPool = 256;
// Textures freed while a frame may still sample them are destroyed this many
// frames later (frames in flight + one of margin).
constexpr std::uint64_t kDestroyDelayFrames = 4;

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

/** Halve an sRGB RGBA8 image with a 2x2 box filter in LINEAR space, so
 *  downscaled colours keep their brightness instead of darkening. */
void halve_srgb(std::vector<std::uint8_t>& px, std::uint32_t& w, std::uint32_t& h) {
    static float lut[256];
    static bool lut_ready = false;
    if (!lut_ready) {
        for (int i = 0; i < 256; ++i) lut[i] = srgb_to_linear(static_cast<float>(i) / 255.0f);
        lut_ready = true;
    }
    const std::uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(nw) * nh * 4);
    for (std::uint32_t y = 0; y < nh; ++y) {
        for (std::uint32_t x = 0; x < nw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = std::min(w - 1, x * 2 + dx);
                    const std::uint32_t sy = std::min(h - 1, y * 2 + dy);
                    const std::uint8_t* s = &px[(static_cast<std::size_t>(sy) * w + sx) * 4];
                    acc[0] += lut[s[0]];
                    acc[1] += lut[s[1]];
                    acc[2] += lut[s[2]];
                    acc[3] += static_cast<float>(s[3]) / 255.0f;
                }
            }
            std::uint8_t* d = &out[(static_cast<std::size_t>(y) * nw + x) * 4];
            for (int k = 0; k < 3; ++k) {
                d[k] = static_cast<std::uint8_t>(
                    std::lround(linear_to_srgb(acc[k] * 0.25f) * 255.0f));
            }
            d[3] = static_cast<std::uint8_t>(std::lround(acc[3] * 0.25f * 255.0f));
        }
    }
    px.swap(out);
    w = nw;
    h = nh;
}

VkSamplerAddressMode address_mode(int gl_wrap) {
    switch (gl_wrap) {
        case 33071: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 33648: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

bool format_sampleable(VkPhysicalDevice pd, VkFormat f) {
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

}  // namespace

std::string TextureCache::sampler_key(const std::string& key,
                                      const gltf_mesh_load::SamplerInfo& s) {
    return key + "|" + std::to_string(s.mag_filter) + "," + std::to_string(s.min_filter) +
           "," + std::to_string(s.wrap_s) + "," + std::to_string(s.wrap_t);
}

bool TextureCache::initialize(VulkanContext& vk, bool anisotropy) {
    vk_ = &vk;
    anisotropy_ = anisotropy;
    VkPhysicalDevice pd = vk.physical_device();
    astc_ = vk.astc_ldr_enabled() && format_sampleable(pd, VK_FORMAT_ASTC_6x6_SRGB_BLOCK) &&
            format_sampleable(pd, VK_FORMAT_ASTC_4x4_SRGB_BLOCK);
    {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8G8B8A8_SRGB, &p);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                          VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        blit_srgb_ = (p.optimalTilingFeatures & need) == need;
    }

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(vk.device(), &lci, nullptr, &set_layout_) != VK_SUCCESS) {
        return false;
    }
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
                VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = vk.graphics_queue_family();
    if (vkCreateCommandPool(vk.device(), &cpi, nullptr, &cmd_pool_) != VK_SUCCESS) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(vk.device(), &fci, nullptr, &fence_) != VK_SUCCESS) return false;

    const std::uint8_t white[4] = {255, 255, 255, 255};
    if (!make_solid(white, 1, 1, white_set_, white_)) return false;
    // 8x8 magenta / near-black checker: unmistakable, and never confused
    // with a texture that is legitimately white or legitimately dark.
    std::vector<std::uint8_t> checker(8 * 8 * 4);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            std::uint8_t* p = &checker[static_cast<std::size_t>(y * 8 + x) * 4];
            const bool on = ((x / 2) + (y / 2)) % 2 == 0;
            p[0] = on ? 255 : 20;
            p[1] = on ? 0 : 0;
            p[2] = on ? 255 : 20;
            p[3] = 255;
        }
    }
    if (!make_solid(checker.data(), 8, 8, missing_set_, missing_)) return false;
    PTV_INFO("textures: ASTC {} | mip blit {} | anisotropy {}",
             astc_.load() ? "yes" : "NO (PNG/JPEG fallback decoded on device)",
             blit_srgb_ ? "yes" : "no", anisotropy_ ? "4x" : "off");
    return true;
}

bool TextureCache::ensure_staging(std::size_t bytes) {
    const std::size_t want = std::max(kStagingBytes, bytes);
    if (staging_ && staging_size_ >= want) return true;
    if (staging_) {
        vmaDestroyBuffer(vk_->allocator(), staging_, staging_alloc_);
        staging_ = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = want;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vk_->allocator(), &bci, &aci, &staging_, &staging_alloc_, &info) !=
        VK_SUCCESS) {
        PTV_ERROR("textures: could not allocate a {} MB staging buffer", want >> 20);
        staging_ = VK_NULL_HANDLE;
        return false;
    }
    staging_mapped_ = info.pMappedData;
    staging_size_ = want;
    return true;
}

bool TextureCache::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(key_mutex_);
    return known_keys_.count(key) != 0;
}

bool TextureCache::decode(const TextureRequest& req, TextureData& out, std::string& err,
                          std::uint32_t max_dim) const {
    out = TextureData{};
    VkPhysicalDevice pd = vk_ ? vk_->physical_device() : VK_NULL_HANDLE;
    auto try_ktx2 = [&](const std::string& path) -> bool {
        if (path.empty() || !astc_.load()) return false;
        if (!asset_source().exists(path)) return false;
        auto bytes = asset_source().map(path);
        if (!bytes) return false;
        ktx2::Info info;
        std::string perr;
        if (!ktx2::parse(bytes->data, bytes->size, info, perr)) {
            PTV_WARN("textures: {} unusable ({}); using the PNG/JPEG", path, perr);
            return false;
        }
        const auto fmt = static_cast<VkFormat>(info.vk_format);
        if (pd != VK_NULL_HANDLE && !format_sampleable(pd, fmt)) {
            PTV_WARN("textures: {} uses format {} this GPU cannot sample", path,
                     static_cast<int>(fmt));
            return false;
        }
        out.format = fmt;
        out.width = info.width;
        out.height = info.height;
        for (const auto& lv : info.levels) {
            out.levels.push_back({lv.offset, lv.size, lv.width, lv.height});
        }
        out.mapped = std::move(*bytes);
        out.source = "ktx2";
        return true;
    };
    if (try_ktx2(req.ktx2_path)) return true;
    if (!req.embedded_hash.empty() && !req.glb_dir.empty() &&
        try_ktx2(req.glb_dir + "/ptv_textures/" + req.embedded_hash + ".ktx2")) {
        return true;
    }

    std::vector<std::uint8_t> encoded;
    std::string source = "embedded";
    if (!req.embedded.empty()) {
        encoded.assign(req.embedded.begin(), req.embedded.end());
    } else if (!req.fallback_path.empty()) {
        auto bytes = asset_source().read_bytes(req.fallback_path);
        if (!bytes) {
            err = "image file not found: " + req.fallback_path +
                  (req.ktx2_path.empty() ? "" : " (and no usable KTX2 " + req.ktx2_path + ")");
            return false;
        }
        encoded = std::move(*bytes);
        const auto dot = req.fallback_path.find_last_of('.');
        source = dot == std::string::npos ? "image" : req.fallback_path.substr(dot + 1);
    } else {
        err = req.ktx2_path.empty() ? "image has neither bytes nor a uri"
                                    : "KTX2 " + req.ktx2_path +
                                          " unavailable and no PNG/JPEG fallback packaged";
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w,
                                        &h, &comp, 4);
    if (px == nullptr || w <= 0 || h <= 0) {
        err = std::string("image decode failed: ") +
              (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (px) stbi_image_free(px);
        return false;
    }
    out.bytes.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    auto uw = static_cast<std::uint32_t>(w);
    auto uh = static_cast<std::uint32_t>(h);
    while (max_dim > 0 && (uw > max_dim || uh > max_dim)) halve_srgb(out.bytes, uw, uh);
    out.format = VK_FORMAT_R8G8B8A8_SRGB;
    out.width = uw;
    out.height = uh;
    out.levels.push_back({0, out.bytes.size(), uw, uh});
    out.generate_mips = true;
    out.source = source;
    return true;
}

VkSampler TextureCache::sampler_for(const gltf_mesh_load::SamplerInfo& s,
                                    std::uint32_t mip_levels) {
    const bool nearest_mag = s.mag_filter == 9728;
    const bool nearest_min = s.min_filter == 9728 || s.min_filter == 9984 ||
                             s.min_filter == 9986;
    const bool no_mips = s.min_filter == 9728 || s.min_filter == 9729 || mip_levels <= 1;
    const std::string key = std::to_string(s.mag_filter) + "," + std::to_string(s.min_filter) +
                            "," + std::to_string(s.wrap_s) + "," + std::to_string(s.wrap_t) +
                            (no_mips ? ",nm" : ",m");
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = nearest_mag ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = nearest_min ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    // Trilinear unless the material asked otherwise: Meta recommends it for
    // VR, where head motion makes mip transitions visible.
    ci.mipmapMode = (s.min_filter == 9984 || s.min_filter == 9985)
                        ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = address_mode(s.wrap_s);
    ci.addressModeV = address_mode(s.wrap_t);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.minLod = 0.0f;
    ci.maxLod = no_mips ? 0.0f : VK_LOD_CLAMP_NONE;
    ci.anisotropyEnable = (anisotropy_ && !nearest_min) ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = ci.anisotropyEnable ? 4.0f : 1.0f;
    VkSampler smp = VK_NULL_HANDLE;
    if (vkCreateSampler(vk_->device(), &ci, nullptr, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    samplers_[key] = smp;
    return smp;
}

VkDescriptorSet TextureCache::allocate_set(VkImageView view, VkSampler sampler,
                                           VkDescriptorPool& out_pool) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    auto try_alloc = [&](VkDescriptorPool pool) -> bool {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        return vkAllocateDescriptorSets(vk_->device(), &ai, &set) == VK_SUCCESS;
    };
    for (auto pool : pools_) {
        if (try_alloc(pool)) {
            out_pool = pool;
            break;
        }
    }
    if (set == VK_NULL_HANDLE) {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetsPerPool};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = kSetsPerPool;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(vk_->device(), &pci, nullptr, &pool) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        pools_.push_back(pool);
        if (!try_alloc(pool)) return VK_NULL_HANDLE;
        out_pool = pool;
    }
    VkDescriptorImageInfo ii{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk_->device(), 1, &w, 0, nullptr);
    return set;
}

bool TextureCache::create_image(TextureData& data, Entry& e, std::string& err) {
    VkDevice dev = vk_->device();
    std::uint32_t mip_levels = static_cast<std::uint32_t>(data.levels.size());
    const bool gen = data.generate_mips && blit_srgb_ && data.format == VK_FORMAT_R8G8B8A8_SRGB;
    if (gen) {
        mip_levels = 1;
        std::uint32_t d = std::max(data.width, data.height);
        while (d > 1) {
            d >>= 1;
            ++mip_levels;
        }
    }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = data.format;
    ici.extent = {data.width, data.height, 1};
    ici.mipLevels = mip_levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (gen ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // Large images get their own allocation so evicting one returns its
    // memory to the system instead of leaving a hole in a shared block.
    std::uint64_t estimate = 0;
    for (const auto& lv : data.levels) estimate += lv.size;
    if (gen) estimate = estimate * 4 / 3;
    if (estimate >= 4ull * 1024 * 1024) aci.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo ainfo{};
    if (vmaCreateImage(vk_->allocator(), &ici, &aci, &e.image, &e.alloc, &ainfo) !=
        VK_SUCCESS) {
        err = "vmaCreateImage failed (out of memory?)";
        return false;
    }
    e.bytes = ainfo.size;

    // Copy every provided level through the staging ring.
    std::size_t total = 0;
    for (const auto& lv : data.levels) total += ((lv.size + 15) & ~std::size_t(15));
    if (!ensure_staging(total)) {
        vmaDestroyImage(vk_->allocator(), e.image, e.alloc);
        e.image = VK_NULL_HANDLE;
        err = "no staging memory";
        return false;
    }
    std::vector<VkBufferImageCopy> regions;
    std::size_t cursor = 0;
    for (std::uint32_t i = 0; i < data.levels.size(); ++i) {
        const auto& lv = data.levels[i];
        std::memcpy(static_cast<std::uint8_t*>(staging_mapped_) + cursor,
                    data.data() + lv.offset, lv.size);
        VkBufferImageCopy r{};
        r.bufferOffset = cursor;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        r.imageExtent = {lv.width, lv.height, 1};
        regions.push_back(r);
        cursor += (lv.size + 15) & ~std::size_t(15);
    }
    vmaFlushAllocation(vk_->allocator(), staging_alloc_, 0, cursor);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) {
        err = "command buffer allocation failed";
        return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = e.image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    vkCmdCopyBufferToImage(cmd, staging_, e.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()), regions.data());
    if (gen) {
        std::int32_t mw = static_cast<std::int32_t>(data.width);
        std::int32_t mh = static_cast<std::int32_t>(data.height);
        for (std::uint32_t i = 1; i < mip_levels; ++i) {
            VkImageMemoryBarrier b = bar;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1};
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &b);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
            blit.srcOffsets[1] = {mw, mh, 1};
            const std::int32_t nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            blit.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cmd, e.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
            mw = nw;
            mh = nh;
        }
        VkImageMemoryBarrier last = bar;
        last.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip_levels - 1, 1, 0, 1};
        last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &last);
    } else {
        VkImageMemoryBarrier b = bar;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &b);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkResetFences(dev, 1, &fence_);
    vkQueueSubmit(vk_->graphics_queue(), 1, &si, fence_);
    vkWaitForFences(dev, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(dev, cmd_pool_, 1, &cmd);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = e.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = data.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
    if (vkCreateImageView(dev, &vci, nullptr, &e.view) != VK_SUCCESS) {
        err = "vkCreateImageView failed";
        return false;
    }
    data.levels.resize(mip_levels);  // report the real level count
    ++stats_.uploads;
    return true;
}

bool TextureCache::make_solid(const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h,
                              VkDescriptorSet& out_set, Entry& out_entry) {
    TextureData d;
    d.format = VK_FORMAT_R8G8B8A8_SRGB;
    d.width = w;
    d.height = h;
    d.bytes.assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
    d.levels.push_back({0, d.bytes.size(), w, h});
    std::string err;
    if (!create_image(d, out_entry, err)) {
        PTV_ERROR("textures: built-in texture failed: {}", err);
        return false;
    }
    gltf_mesh_load::SamplerInfo s;
    s.mag_filter = 9728;  // nearest: the checker must stay crisp
    s.min_filter = 9728;
    VkSampler smp = sampler_for(s, 1);
    out_set = allocate_set(out_entry.view, smp, out_entry.pool);
    out_entry.set = out_set;
    out_entry.live = true;
    return out_set != VK_NULL_HANDLE;
}

int TextureCache::acquire(const std::string& key, const gltf_mesh_load::SamplerInfo& sampler) {
    auto it = by_key_.find(sampler_key(key, sampler));
    if (it == by_key_.end()) return -1;
    Entry& e = entries_[static_cast<std::size_t>(it->second)];
    if (!e.live) return -1;
    ++e.refs;
    return it->second;
}

int TextureCache::upload(const std::string& key, TextureData&& data,
                         const gltf_mesh_load::SamplerInfo& sampler) {
    if (!vk_) return -1;
    if (const int existing = acquire(key, sampler); existing >= 0) return existing;
    Entry e;
    e.key = key;
    std::string err;
    const std::string source = data.source;
    if (!create_image(data, e, err)) {
        PTV_ERROR("textures: upload of {} failed: {}", key, err);
        destroy_entry(e);
        return -1;
    }
    VkSampler smp = sampler_for(sampler, static_cast<std::uint32_t>(data.levels.size()));
    e.set = allocate_set(e.view, smp, e.pool);
    if (e.set == VK_NULL_HANDLE) {
        PTV_ERROR("textures: no descriptor set for {}", key);
        destroy_entry(e);
        return -1;
    }
    e.refs = 1;
    e.live = true;
    int id;
    if (!free_ids_.empty()) {
        id = free_ids_.back();
        free_ids_.pop_back();
        entries_[static_cast<std::size_t>(id)] = std::move(e);
    } else {
        id = static_cast<int>(entries_.size());
        entries_.push_back(std::move(e));
    }
    by_key_[sampler_key(key, sampler)] = id;
    {
        std::lock_guard<std::mutex> lock(key_mutex_);
        known_keys_.insert(key);
    }
    ++stats_.resident;
    stats_.bytes += entries_[static_cast<std::size_t>(id)].bytes;
    if (source == "ktx2") {
        ++stats_.ktx2;
    } else {
        ++stats_.decoded;
    }
    return id;
}

void TextureCache::release(int id) {
    if (id < 0 || static_cast<std::size_t>(id) >= entries_.size()) return;
    Entry& e = entries_[static_cast<std::size_t>(id)];
    if (!e.live || e.refs <= 0) return;
    if (--e.refs == 0) e.released_frame = frame_index_;
}

void TextureCache::collect(std::uint64_t frame_index) {
    frame_index_ = frame_index;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        if (!e.live || e.refs > 0) continue;
        if (frame_index < e.released_frame + kDestroyDelayFrames) continue;
        for (auto it = by_key_.begin(); it != by_key_.end();) {
            it = (it->second == static_cast<int>(i)) ? by_key_.erase(it) : std::next(it);
        }
        {
            std::lock_guard<std::mutex> lock(key_mutex_);
            known_keys_.erase(e.key);
        }
        stats_.bytes -= std::min(stats_.bytes, e.bytes);
        if (stats_.resident > 0) --stats_.resident;
        destroy_entry(e);
        free_ids_.push_back(static_cast<int>(i));
    }
}

VkDescriptorSet TextureCache::descriptor(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= entries_.size()) return white_set_;
    const Entry& e = entries_[static_cast<std::size_t>(id)];
    return e.live ? e.set : white_set_;
}

std::uint64_t TextureCache::texture_bytes(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= entries_.size()) return 0;
    return entries_[static_cast<std::size_t>(id)].bytes;
}

void TextureCache::destroy_entry(Entry& e) {
    if (!vk_) return;
    if (e.set && e.pool) vkFreeDescriptorSets(vk_->device(), e.pool, 1, &e.set);
    if (e.view) vkDestroyImageView(vk_->device(), e.view, nullptr);
    if (e.image) vmaDestroyImage(vk_->allocator(), e.image, e.alloc);
    e = Entry{};
}

void TextureCache::shutdown() {
    if (!vk_) return;
    VkDevice dev = vk_->device();
    for (auto& e : entries_) destroy_entry(e);
    entries_.clear();
    destroy_entry(white_);
    destroy_entry(missing_);
    for (auto& kv : samplers_) vkDestroySampler(dev, kv.second, nullptr);
    samplers_.clear();
    for (auto p : pools_) vkDestroyDescriptorPool(dev, p, nullptr);
    pools_.clear();
    if (staging_) vmaDestroyBuffer(vk_->allocator(), staging_, staging_alloc_);
    staging_ = VK_NULL_HANDLE;
    if (fence_) vkDestroyFence(dev, fence_, nullptr);
    fence_ = VK_NULL_HANDLE;
    if (cmd_pool_) vkDestroyCommandPool(dev, cmd_pool_, nullptr);
    cmd_pool_ = VK_NULL_HANDLE;
    if (set_layout_) vkDestroyDescriptorSetLayout(dev, set_layout_, nullptr);
    set_layout_ = VK_NULL_HANDLE;
    by_key_.clear();
    free_ids_.clear();
    {
        std::lock_guard<std::mutex> lock(key_mutex_);
        known_keys_.clear();
    }
    stats_ = Stats{};
    vk_ = nullptr;
}

}  // namespace ptv
