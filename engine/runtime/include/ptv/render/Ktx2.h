#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptv::ktx2 {

/** One mip level inside a KTX2 file. Level 0 is the full-size image. */
struct Level {
    std::size_t offset{0};
    std::size_t size{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

/** The parts of a KTX2 header the runtime needs to upload an image as is.
 *  Only uncompressed containers (supercompression 0) of a single 2D layer
 *  and face are accepted -- the shape ``ptv_texture_compress`` writes. */
struct Info {
    std::uint32_t vk_format{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<Level> levels;
};

/** Parse ``bytes`` (a whole .ktx2 file). False with ``err`` on anything the
 *  runtime cannot upload directly. */
bool parse(const std::uint8_t* bytes, std::size_t size, Info& out, std::string& err);

/** Bytes of one ASTC-compressed level: 16 per block. */
std::size_t astc_level_bytes(std::uint32_t width, std::uint32_t height,
                             std::uint32_t block_w, std::uint32_t block_h);

/** Block dimensions of a VK_FORMAT_ASTC_* value (0 for other formats). */
void astc_block_size(std::uint32_t vk_format, std::uint32_t& bw, std::uint32_t& bh);

/** Serialise an uncompressed KTX2 (used by tests to build fixtures). Level
 *  data must be given largest first. ``dfd`` may be empty. */
std::vector<std::uint8_t> write(std::uint32_t vk_format, std::uint32_t width,
                                std::uint32_t height,
                                const std::vector<std::vector<std::uint8_t>>& levels);

}  // namespace ptv::ktx2
