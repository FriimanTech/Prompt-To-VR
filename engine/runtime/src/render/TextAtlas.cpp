#include "ptv/render/TextAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"

namespace ptv {
namespace {

constexpr int kFirst = 32;
constexpr int kLast = 126;
constexpr int kCount = kLast - kFirst + 1;
constexpr int kAtlasWidth = 1024;

}  // namespace

std::string TextAtlas::default_font_path() {
    return std::string(PTV_ENGINE_ASSET_DIR) + "/fonts/AtkinsonHyperlegible-Bold.ttf";
}

bool TextAtlas::build_default(std::string& err) {
    const std::string path = default_font_path();
    auto bytes = asset_source().read_bytes(path);
    if (!bytes || bytes->empty()) {
        err = "font not found: " + path;
        return false;
    }
    std::vector<unsigned char> ttf(bytes->begin(), bytes->end());
    return build(ttf, err);
}

bool TextAtlas::build(const std::vector<unsigned char>& ttf, std::string& err,
                      float pixel_height, int padding) {
    stbtt_fontinfo info{};
    if (ttf.empty() ||
        !stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        err = "not a TrueType font";
        return false;
    }
    const float scale = stbtt_ScaleForPixelHeight(&info, pixel_height);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    line_px_ = static_cast<float>(ascent - descent + gap) * scale;
    int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
    if (stbtt_GetCodepointBox(&info, 'H', &cx0, &cy0, &cx1, &cy1)) {
        cap_px_ = std::max(1.0f, static_cast<float>(cy1) * scale);
    } else {
        cap_px_ = 0.7f * pixel_height;
    }

    // onedge 180 with 7 px of padding leaves room for the outline the shader
    // draws below the edge.
    edge_ = 180.0f;
    const float dist_scale = edge_ / static_cast<float>(padding);

    struct Baked {
        unsigned char* bmp{nullptr};
        int w{0}, h{0}, xoff{0}, yoff{0};
    };
    std::vector<Baked> baked(kCount);
    glyphs_.assign(kCount, Glyph{});
    for (int i = 0; i < kCount; ++i) {
        const int cp = kFirst + i;
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
        glyphs_[static_cast<size_t>(i)].advance = static_cast<float>(adv) * scale;
        Baked& b = baked[static_cast<size_t>(i)];
        b.bmp = stbtt_GetCodepointSDF(&info, scale, cp, padding,
                                      static_cast<unsigned char>(edge_), dist_scale,
                                      &b.w, &b.h, &b.xoff, &b.yoff);
    }

    // Shelf packing into a fixed-width atlas; the height grows to fit.
    int x = 1, y = 1, shelf = 0;
    for (int i = 0; i < kCount; ++i) {
        Baked& b = baked[static_cast<size_t>(i)];
        if (b.bmp == nullptr) continue;  // space: no bitmap
        if (x + b.w + 1 > kAtlasWidth) {
            x = 1;
            y += shelf + 1;
            shelf = 0;
        }
        Glyph& g = glyphs_[static_cast<size_t>(i)];
        g.x = x;
        g.y = y;
        g.w = b.w;
        g.h = b.h;
        g.xoff = static_cast<float>(b.xoff);
        g.yoff = static_cast<float>(b.yoff);
        x += b.w + 1;
        shelf = std::max(shelf, b.h);
    }
    int height = 1;
    while (height < y + shelf + 1) height *= 2;
    width_ = kAtlasWidth;
    height_ = height;
    pixels_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
    for (int i = 0; i < kCount; ++i) {
        Baked& b = baked[static_cast<size_t>(i)];
        if (b.bmp == nullptr) continue;
        const Glyph& g = glyphs_[static_cast<size_t>(i)];
        for (int r = 0; r < b.h; ++r) {
            std::memcpy(&pixels_[static_cast<size_t>(g.y + r) * width_ + g.x],
                        b.bmp + static_cast<size_t>(r) * b.w, static_cast<size_t>(b.w));
        }
        stbtt_FreeSDF(b.bmp, nullptr);
    }

    kern_.assign(static_cast<size_t>(kCount) * kCount, 0.0f);
    for (int a = 0; a < kCount; ++a) {
        for (int c = 0; c < kCount; ++c) {
            const int k = stbtt_GetCodepointKernAdvance(&info, kFirst + a, kFirst + c);
            if (k != 0) kern_[static_cast<size_t>(a) * kCount + c] = static_cast<float>(k) * scale;
        }
    }
    PTV_INFO("text: SDF atlas {}x{} ({} glyphs, cap {:.1f} px)", width_, height_,
             kCount, cap_px_);
    return true;
}

const TextAtlas::Glyph& TextAtlas::glyph(char c) const {
    int i = static_cast<unsigned char>(c);
    if (i < kFirst || i > kLast) i = '?';
    return glyphs_[static_cast<size_t>(i - kFirst)];
}

float TextAtlas::kern_px(char a, char b) const {
    const int ia = static_cast<unsigned char>(a), ib = static_cast<unsigned char>(b);
    if (ia < kFirst || ia > kLast || ib < kFirst || ib > kLast || kern_.empty()) return 0.0f;
    return kern_[static_cast<size_t>(ia - kFirst) * kCount + (ib - kFirst)];
}

float TextAtlas::line_width_px(std::string_view line) const {
    float w = 0.0f;
    for (size_t i = 0; i < line.size(); ++i) {
        w += glyph(line[i]).advance;
        if (i + 1 < line.size()) w += kern_px(line[i], line[i + 1]);
    }
    return w;
}

float TextAtlas::line_height(float cap_height_m) const {
    return line_px_ * (cap_height_m / cap_px_);
}

float TextAtlas::measure(std::string_view text, float cap_height_m) const {
    if (!valid()) return 0.0f;
    float widest = 0.0f;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        widest = std::max(widest, line_width_px(text.substr(start, end - start)));
        start = end + 1;
    }
    return widest * (cap_height_m / cap_px_);
}

void TextAtlas::layout(std::string_view text, float cap_height_m, int align,
                       std::vector<TextQuad>& out) const {
    if (!valid() || cap_height_m <= 0.0f) return;
    const float k = cap_height_m / cap_px_;
    const float iw = 1.0f / static_cast<float>(width_);
    const float ih = 1.0f / static_cast<float>(height_);
    float baseline = 0.0f;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view line = text.substr(start, end - start);
        const float w = line_width_px(line);
        float pen = align == 1 ? -0.5f * w : (align == 2 ? -w : 0.0f);
        for (size_t i = 0; i < line.size(); ++i) {
            const Glyph& g = glyph(line[i]);
            if (g.w > 0 && g.h > 0) {
                TextQuad q{};
                const float left = (pen + g.xoff) * k;
                const float top = baseline - g.yoff * k;
                q.p0 = Vec2(left, top - static_cast<float>(g.h) * k);
                q.p1 = Vec2(left + static_cast<float>(g.w) * k, top);
                q.uv0 = Vec2(static_cast<float>(g.x) * iw,
                             static_cast<float>(g.y + g.h) * ih);
                q.uv1 = Vec2(static_cast<float>(g.x + g.w) * iw,
                             static_cast<float>(g.y) * ih);
                out.push_back(q);
            }
            pen += g.advance;
            if (i + 1 < line.size()) pen += kern_px(line[i], line[i + 1]);
        }
        baseline -= line_px_ * k;
        start = end + 1;
    }
}

}  // namespace ptv
