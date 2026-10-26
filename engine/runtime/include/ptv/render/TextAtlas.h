#pragma once

// Signed-distance-field glyph atlas for world-space text.
//
// The engine had no way to draw a single letter, so every menu was a column
// of unlabeled rectangles and a Game Over screen could not say "Game Over".
// This bakes one SDF atlas at start-up from the engine's bundled font
// (Atkinson Hyperlegible, SIL OFL 1.1, chosen for legibility) with
// stb_truetype, and lays strings out as textured quads. SDF rather than a
// plain bitmap because VR text is seen at every distance and angle: a
// distance field stays sharp when magnified and does not shimmer when small.
//
// CPU-only and GPU-free, so the layout is unit-testable; TextRenderer uploads
// ``pixels`` and draws the quads.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv {

/** One glyph quad in the text's own plane: +X reading direction, +Y up, the
 *  baseline of the first line at y = 0. Metres. */
struct TextQuad {
    Vec2 p0{0.0f};  // bottom-left
    Vec2 p1{0.0f};  // top-right
    Vec2 uv0{0.0f};  // matches p0
    Vec2 uv1{0.0f};  // matches p1
};

class TextAtlas {
public:
    /** Bake ASCII 32..126 from a TrueType font. */
    bool build(const std::vector<unsigned char>& ttf, std::string& err,
               float pixel_height = 56.0f, int padding = 7);

    /** Bake from the engine's bundled font (``PTV_ENGINE_ASSET_DIR``/fonts),
     *  read through the asset source so it works inside the Quest APK. */
    bool build_default(std::string& err);

    bool valid() const { return width_ > 0 && !glyphs_.empty(); }
    int width() const { return width_; }
    int height() const { return height_; }
    /** R8, ``width * height`` bytes, row-major, top row first. */
    const std::vector<std::uint8_t>& pixels() const { return pixels_; }
    /** The distance value (0..1) that lies exactly on a glyph's edge. */
    float edge_value() const { return edge_ / 255.0f; }

    /** Lay ``utf8`` out with capital letters ``cap_height_m`` tall. Lines
     *  split on '\n' and are aligned individually (0 left, 1 centre, 2
     *  right). Characters outside ASCII 32..126 draw as '?'. */
    void layout(std::string_view utf8, float cap_height_m, int align,
                std::vector<TextQuad>& out) const;
    /** Width of the widest line, metres. */
    float measure(std::string_view utf8, float cap_height_m) const;
    /** Distance between baselines, metres, for ``cap_height_m``. */
    float line_height(float cap_height_m) const;

    static std::string default_font_path();

private:
    struct Glyph {
        // Atlas rectangle, pixels.
        int x{0}, y{0}, w{0}, h{0};
        // Offset of the bitmap's top-left from the pen, pixels (y down).
        float xoff{0.0f}, yoff{0.0f};
        float advance{0.0f};
    };
    const Glyph& glyph(char c) const;
    float line_width_px(std::string_view line) const;
    float kern_px(char a, char b) const;

    std::vector<Glyph> glyphs_;  // index = c - 32
    std::vector<float> kern_;    // 95 x 95, pixels
    std::vector<std::uint8_t> pixels_;
    int width_{0};
    int height_{0};
    float cap_px_{1.0f};
    float line_px_{1.0f};
    float edge_{180.0f};
};

}  // namespace ptv
