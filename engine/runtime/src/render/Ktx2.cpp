#include "ptv/render/Ktx2.h"

#include <algorithm>
#include <cstring>

namespace ptv::ktx2 {
namespace {

// KTX 2.0 file identifier: «KTX 20»\r\n\x1A\n
constexpr std::uint8_t kIdentifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                          0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::uint64_t rd64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
void wr32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
}
void wr64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
}

}  // namespace

void astc_block_size(std::uint32_t f, std::uint32_t& bw, std::uint32_t& bh) {
    bw = bh = 0;
    // VK_FORMAT_ASTC_4x4_UNORM_BLOCK = 157 .. VK_FORMAT_ASTC_12x12_SRGB_BLOCK = 184,
    // UNORM/SRGB pairs in this order.
    static const std::uint32_t dims[14][2] = {{4, 4},  {5, 4},  {5, 5},   {6, 5},  {6, 6},
                                              {8, 5},  {8, 6},  {8, 8},   {10, 5}, {10, 6},
                                              {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    if (f >= 157 && f <= 184) {
        const std::uint32_t i = (f - 157) / 2;
        bw = dims[i][0];
        bh = dims[i][1];
    }
}

std::size_t astc_level_bytes(std::uint32_t w, std::uint32_t h, std::uint32_t bw,
                             std::uint32_t bh) {
    if (bw == 0 || bh == 0) return 0;
    const std::size_t bx = (w + bw - 1) / bw;
    const std::size_t by = (h + bh - 1) / bh;
    return bx * by * 16;
}

bool parse(const std::uint8_t* bytes, std::size_t size, Info& out, std::string& err) {
    out = Info{};
    if (bytes == nullptr || size < 80) {
        err = "file shorter than a KTX2 header";
        return false;
    }
    if (std::memcmp(bytes, kIdentifier, 12) != 0) {
        err = "not a KTX2 file (bad identifier)";
        return false;
    }
    out.vk_format = rd32(bytes + 12);
    const std::uint32_t type_size = rd32(bytes + 16);
    out.width = rd32(bytes + 20);
    out.height = rd32(bytes + 24);
    const std::uint32_t depth = rd32(bytes + 28);
    const std::uint32_t layers = rd32(bytes + 32);
    const std::uint32_t faces = rd32(bytes + 36);
    const std::uint32_t level_count = std::max<std::uint32_t>(1, rd32(bytes + 40));
    const std::uint32_t supercompression = rd32(bytes + 44);
    (void)type_size;
    if (out.vk_format == 0) {
        err = "KTX2 with VK_FORMAT_UNDEFINED (Basis) is not supported";
        return false;
    }
    if (supercompression != 0) {
        err = "supercompressed KTX2 (scheme " + std::to_string(supercompression) +
              ") is not supported";
        return false;
    }
    if (depth > 1 || layers > 1 || faces != 1) {
        err = "only single-layer 2D KTX2 textures are supported";
        return false;
    }
    if (out.width == 0 || out.height == 0) {
        err = "KTX2 has zero size";
        return false;
    }
    const std::size_t index = 80;
    if (index + static_cast<std::size_t>(level_count) * 24 > size) {
        err = "KTX2 level index runs past the end of the file";
        return false;
    }
    for (std::uint32_t i = 0; i < level_count; ++i) {
        const std::uint8_t* e = bytes + index + i * 24;
        Level lv;
        lv.offset = static_cast<std::size_t>(rd64(e));
        lv.size = static_cast<std::size_t>(rd64(e + 8));
        lv.width = std::max<std::uint32_t>(1, out.width >> i);
        lv.height = std::max<std::uint32_t>(1, out.height >> i);
        if (lv.offset + lv.size > size || lv.size == 0) {
            err = "KTX2 level " + std::to_string(i) + " lies outside the file";
            return false;
        }
        std::uint32_t bw = 0, bh = 0;
        astc_block_size(out.vk_format, bw, bh);
        if (bw != 0 && astc_level_bytes(lv.width, lv.height, bw, bh) != lv.size) {
            err = "KTX2 level " + std::to_string(i) + " has the wrong size for its format";
            return false;
        }
        out.levels.push_back(lv);
    }
    return true;
}

std::vector<std::uint8_t> write(std::uint32_t vk_format, std::uint32_t width,
                                std::uint32_t height,
                                const std::vector<std::vector<std::uint8_t>>& levels) {
    std::vector<std::uint8_t> b(kIdentifier, kIdentifier + 12);
    wr32(b, vk_format);
    wr32(b, 1);  // typeSize (1 for block-compressed formats)
    wr32(b, width);
    wr32(b, height);
    wr32(b, 0);  // pixelDepth
    wr32(b, 0);  // layerCount
    wr32(b, 1);  // faceCount
    wr32(b, static_cast<std::uint32_t>(levels.size()));
    wr32(b, 0);  // supercompressionScheme
    // Index: dfd, kvd (none), sgd (none).
    const std::size_t level_index = 80;
    const std::size_t after_index = level_index + levels.size() * 24;
    wr32(b, 0);
    wr32(b, 0);
    wr32(b, 0);
    wr32(b, 0);
    wr64(b, 0);
    wr64(b, 0);
    // Level data is stored smallest first in the file (spec), each aligned to
    // lcm(texel block size, 4) = 16 for ASTC.
    std::vector<std::size_t> offsets(levels.size(), 0);
    std::size_t cursor = after_index;
    for (std::size_t i = levels.size(); i-- > 0;) {
        cursor = (cursor + 15) & ~static_cast<std::size_t>(15);
        offsets[i] = cursor;
        cursor += levels[i].size();
    }
    for (std::size_t i = 0; i < levels.size(); ++i) {
        wr64(b, offsets[i]);
        wr64(b, levels[i].size());
        wr64(b, levels[i].size());
    }
    b.resize(cursor, 0);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        std::memcpy(b.data() + offsets[i], levels[i].data(), levels[i].size());
    }
    return b;
}

}  // namespace ptv::ktx2
