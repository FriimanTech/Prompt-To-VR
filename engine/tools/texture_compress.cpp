// ptv_texture_compress: one base-colour image -> ASTC KTX2 (+ a resized
// PNG/JPEG) for the asset_optimization stage.
//
//   ptv_texture_compress --input <img> --max-size <N>
//                        --out-ktx2 <file.ktx2> [--out-image <file.jpg|png>]
//                        [--block 4x4|5x5|6x6|8x8] [--linear]
//                        [--quality fast|medium|thorough] [--report <r.json>]
//
// * The image is halved (2x2 box filter in LINEAR light for sRGB colour, so
//   downscaled textures keep their brightness) until it fits --max-size.
// * A full mip chain is built the same way; every level is ASTC-encoded by
//   libktx (ARM astcenc) into an uncompressed (no supercompression) KTX2,
//   exactly the shape the runtime's KTX2 reader uploads directly. Meta's
//   Quest guidance: ASTC, mipmaps on all 3D textures, 6x6 as the default
//   block, 4x4 for UI / text.
// * --out-image writes the resized level 0 as PNG (when the image has alpha
//   or the name ends in .png) or JPEG quality 92 -- the PC VR fallback,
//   since desktop GPUs do not sample ASTC.
// * The written KTX2 is re-read with the runtime's parser before success is
//   reported, so a file the headset cannot load never leaves this tool.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <ktx.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "ptv/render/Ktx2.h"

namespace {

// VkFormat values (vulkan_core.h); ASTC SRGB = UNORM + 1.
constexpr std::uint32_t kVkR8G8B8A8Unorm = 37;
constexpr std::uint32_t kVkR8G8B8A8Srgb = 43;

struct Options {
    std::string input;
    std::string out_ktx2;
    std::string out_image;
    std::string report;
    std::uint32_t max_size = 1024;
    std::string block = "6x6";
    bool linear = false;
    std::string quality = "medium";
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--input") {
            if (!next(o.input)) return false;
        } else if (a == "--out-ktx2") {
            if (!next(o.out_ktx2)) return false;
        } else if (a == "--out-image") {
            if (!next(o.out_image)) return false;
        } else if (a == "--report") {
            if (!next(o.report)) return false;
        } else if (a == "--max-size") {
            if (!next(v)) return false;
            o.max_size = static_cast<std::uint32_t>(std::stoul(v));
        } else if (a == "--block") {
            if (!next(o.block)) return false;
        } else if (a == "--quality") {
            if (!next(o.quality)) return false;
        } else if (a == "--linear") {
            o.linear = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return !o.input.empty() && !o.out_ktx2.empty() && o.max_size >= 4;
}

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct Image {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> px;  // RGBA8
};

Image halve(const Image& in, bool srgb) {
    static float lut[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; ++i) lut[i] = srgb_to_linear(static_cast<float>(i) / 255.0f);
        ready = true;
    }
    Image out;
    out.w = std::max(1u, in.w / 2);
    out.h = std::max(1u, in.h / 2);
    out.px.resize(static_cast<std::size_t>(out.w) * out.h * 4);
    for (std::uint32_t y = 0; y < out.h; ++y) {
        for (std::uint32_t x = 0; x < out.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint32_t sx = std::min(in.w - 1, x * 2 + dx);
                    const std::uint32_t sy = std::min(in.h - 1, y * 2 + dy);
                    const std::uint8_t* s = &in.px[(static_cast<std::size_t>(sy) * in.w + sx) * 4];
                    for (int k = 0; k < 3; ++k) {
                        acc[k] += srgb ? lut[s[k]] : static_cast<float>(s[k]) / 255.0f;
                    }
                    acc[3] += static_cast<float>(s[3]) / 255.0f;
                }
            }
            std::uint8_t* d = &out.px[(static_cast<std::size_t>(y) * out.w + x) * 4];
            for (int k = 0; k < 3; ++k) {
                const float v = acc[k] * 0.25f;
                d[k] = static_cast<std::uint8_t>(
                    std::lround((srgb ? linear_to_srgb(v) : std::clamp(v, 0.0f, 1.0f)) * 255.0f));
            }
            d[3] = static_cast<std::uint8_t>(std::lround(acc[3] * 0.25f * 255.0f));
        }
    }
    return out;
}

bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: ptv_texture_compress --input <img> --max-size <N> "
                     "--out-ktx2 <f.ktx2> [--out-image <f>] [--block 6x6] [--linear] "
                     "[--quality medium] [--report r.json]\n");
        return 2;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* raw = stbi_load(o.input.c_str(), &w, &h, &comp, 4);
    if (raw == nullptr || w <= 0 || h <= 0) {
        std::fprintf(stderr, "cannot decode %s: %s\n", o.input.c_str(),
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return 1;
    }
    Image base;
    base.w = static_cast<std::uint32_t>(w);
    base.h = static_cast<std::uint32_t>(h);
    base.px.assign(raw, raw + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(raw);
    const bool srgb = !o.linear;
    bool has_alpha = false;
    for (std::size_t i = 3; i < base.px.size(); i += 4) {
        if (base.px[i] < 250) {
            has_alpha = true;
            break;
        }
    }
    const std::uint32_t src_w = base.w, src_h = base.h;
    while (base.w > o.max_size || base.h > o.max_size) base = halve(base, srgb);

    std::vector<Image> levels;
    levels.push_back(base);
    while (levels.back().w > 1 || levels.back().h > 1) levels.push_back(halve(levels.back(), srgb));

    ktxTextureCreateInfo ci{};
    ci.vkFormat = srgb ? kVkR8G8B8A8Srgb : kVkR8G8B8A8Unorm;
    ci.baseWidth = base.w;
    ci.baseHeight = base.h;
    ci.baseDepth = 1;
    ci.numDimensions = 2;
    ci.numLevels = static_cast<ktx_uint32_t>(levels.size());
    ci.numLayers = 1;
    ci.numFaces = 1;
    ci.isArray = KTX_FALSE;
    ci.generateMipmaps = KTX_FALSE;
    ktxTexture2* tex = nullptr;
    if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS) {
        std::fprintf(stderr, "ktxTexture2_Create failed\n");
        return 1;
    }
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (ktxTexture_SetImageFromMemory(ktxTexture(tex), static_cast<ktx_uint32_t>(i), 0, 0,
                                          levels[i].px.data(), levels[i].px.size()) !=
            KTX_SUCCESS) {
            std::fprintf(stderr, "ktxTexture_SetImageFromMemory failed at level %zu\n", i);
            ktxTexture_Destroy(ktxTexture(tex));
            return 1;
        }
    }
    ktxAstcParams p{};
    p.structSize = sizeof(p);
    p.threadCount = std::max(1u, std::thread::hardware_concurrency());
    p.mode = KTX_PACK_ASTC_ENCODER_MODE_LDR;
    p.qualityLevel = o.quality == "fast"       ? KTX_PACK_ASTC_QUALITY_LEVEL_FAST
                     : o.quality == "thorough" ? KTX_PACK_ASTC_QUALITY_LEVEL_THOROUGH
                                               : KTX_PACK_ASTC_QUALITY_LEVEL_MEDIUM;
    p.perceptual = srgb ? KTX_TRUE : KTX_FALSE;
    if (o.block == "4x4") {
        p.blockDimension = KTX_PACK_ASTC_BLOCK_DIMENSION_4x4;
    } else if (o.block == "5x5") {
        p.blockDimension = KTX_PACK_ASTC_BLOCK_DIMENSION_5x5;
    } else if (o.block == "8x8") {
        p.blockDimension = KTX_PACK_ASTC_BLOCK_DIMENSION_8x8;
    } else {
        p.blockDimension = KTX_PACK_ASTC_BLOCK_DIMENSION_6x6;
    }
    if (ktxTexture2_CompressAstcEx(tex, &p) != KTX_SUCCESS) {
        std::fprintf(stderr, "ASTC compression failed\n");
        ktxTexture_Destroy(ktxTexture(tex));
        return 1;
    }
    if (ktxTexture_WriteToNamedFile(ktxTexture(tex), o.out_ktx2.c_str()) != KTX_SUCCESS) {
        std::fprintf(stderr, "cannot write %s\n", o.out_ktx2.c_str());
        ktxTexture_Destroy(ktxTexture(tex));
        return 1;
    }
    const std::uint32_t out_format = tex->vkFormat;
    ktxTexture_Destroy(ktxTexture(tex));

    // Verify with the runtime's own reader: this is the file the headset gets.
    std::ifstream f(o.out_ktx2, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    ptv::ktx2::Info info;
    std::string perr;
    if (!ptv::ktx2::parse(bytes.data(), bytes.size(), info, perr)) {
        std::fprintf(stderr, "written KTX2 does not pass the runtime reader: %s\n",
                     perr.c_str());
        return 1;
    }
    std::uint32_t bw = 0, bh = 0;
    ptv::ktx2::astc_block_size(info.vk_format, bw, bh);
    std::size_t gpu_bytes = 0;
    for (const auto& lv : info.levels) gpu_bytes += lv.size;

    std::size_t image_bytes = 0;
    if (!o.out_image.empty()) {
        const bool png = has_alpha || ends_with(o.out_image, ".png");
        int ok = png ? stbi_write_png(o.out_image.c_str(), static_cast<int>(base.w),
                                      static_cast<int>(base.h), 4, base.px.data(),
                                      static_cast<int>(base.w) * 4)
                     : stbi_write_jpg(o.out_image.c_str(), static_cast<int>(base.w),
                                      static_cast<int>(base.h), 4, base.px.data(), 92);
        if (!ok) {
            std::fprintf(stderr, "cannot write %s\n", o.out_image.c_str());
            return 1;
        }
        std::ifstream g(o.out_image, std::ios::binary | std::ios::ate);
        image_bytes = static_cast<std::size_t>(g.tellg());
    }

    std::printf("%s -> %s  %ux%u -> %ux%u  %zu levels  ASTC %ux%u%s  %zu KB\n", o.input.c_str(),
                o.out_ktx2.c_str(), src_w, src_h, base.w, base.h, info.levels.size(), bw, bh,
                srgb ? " sRGB" : "", gpu_bytes >> 10);
    if (!o.report.empty()) {
        nlohmann::json r;
        r["input"] = o.input;
        r["source_width"] = src_w;
        r["source_height"] = src_h;
        r["width"] = base.w;
        r["height"] = base.h;
        r["levels"] = info.levels.size();
        r["vk_format"] = out_format;
        r["block"] = std::to_string(bw) + "x" + std::to_string(bh);
        r["srgb"] = srgb;
        r["has_alpha"] = has_alpha;
        r["ktx2"] = o.out_ktx2;
        r["ktx2_bytes"] = bytes.size();
        r["gpu_bytes"] = gpu_bytes;
        r["image"] = o.out_image;
        r["image_bytes"] = image_bytes;
        std::ofstream rf(o.report);
        rf << r.dump(2) << "\n";
    }
    return 0;
}
