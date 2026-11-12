#pragma once

// The offline tools' CPU rasteriser: z-buffered, perspective-correct,
// textured Lambert, with a per-pixel id buffer (whatever the caller numbers
// its triangles or objects by) and view-space depth. Shared by ptv_glb_views
// (one GLB, triangle ids) and ptv_scene_views (a whole scene, entity ids), so
// a layout review and a socket render shade exactly alike. Deterministic and
// GPU-free.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace ptv_raster {

struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    glm::vec4 sample(float u, float v) const {
        if (w <= 0 || h <= 0) return glm::vec4(1.0f);
        u -= std::floor(u);
        v -= std::floor(v);
        const float x = u * static_cast<float>(w) - 0.5f;
        const float y = v * static_cast<float>(h) - 0.5f;
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        auto px = [&](int xi, int yi) {
            xi = ((xi % w) + w) % w;
            yi = ((yi % h) + h) % h;
            const unsigned char* p = &rgba[(static_cast<size_t>(yi) * w + xi) * 4];
            return glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
        };
        return glm::mix(glm::mix(px(x0, y0), px(x0 + 1, y0), fx),
                        glm::mix(px(x0, y0 + 1), px(x0 + 1, y0 + 1), fx), fy);
    }
};

struct Camera {
    bool ortho = true;
    glm::vec3 eye{0.0f}, forward{0.0f}, up{0.0f};
    glm::mat4 view{1.0f}, proj{1.0f};
};

struct Target {
    int size = 0;
    std::vector<float> depth;          // NDC z (0 near, 1 far)
    std::vector<float> view_depth;     // distance along forward
    std::vector<uint32_t> ids;         // caller's id + 1 (0 = background)
    std::vector<unsigned char> rgb;
    /** Neutral light grey by default: no colour a material could be confused with. */
    explicit Target(int s, glm::vec3 background = glm::vec3(196, 198, 202) / 255.0f)
        : size(s),
          depth(static_cast<size_t>(s) * s, 1.0f),
          view_depth(static_cast<size_t>(s) * s, 0.0f),
          ids(static_cast<size_t>(s) * s, 0u),
          rgb(static_cast<size_t>(s) * s * 3, 0) {
        const glm::vec3 bg = glm::clamp(background, 0.0f, 1.0f) * 255.0f + 0.5f;
        for (int i = 0; i < s * s; ++i) {
            rgb[i * 3 + 0] = static_cast<unsigned char>(bg.r);
            rgb[i * 3 + 1] = static_cast<unsigned char>(bg.g);
            rgb[i * 3 + 2] = static_cast<unsigned char>(bg.b);
        }
    }
};

struct ClipVert {
    glm::vec4 clip;
    glm::vec3 world;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec4 color;
};

/** Rasterise one triangle. ``id`` is written to the id buffer only when
 *  ``write_id``; ``flat`` overrides shading with a solid colour (markers). */
inline void raster_triangle(Target& t, const Camera& view, const ClipVert v[3],
                            const Image* tex, uint32_t id, bool write_id,
                            const glm::vec3* flat) {
    glm::vec3 ndc[3];
    float inv_w[3];
    for (int i = 0; i < 3; ++i) {
        if (v[i].clip.w <= 1e-6f) return;
        inv_w[i] = 1.0f / v[i].clip.w;
        ndc[i] = glm::vec3(v[i].clip) * inv_w[i];
    }
    const float s = static_cast<float>(t.size);
    glm::vec2 p[3];
    for (int i = 0; i < 3; ++i) {
        p[i] = glm::vec2((ndc[i].x * 0.5f + 0.5f) * s, (1.0f - (ndc[i].y * 0.5f + 0.5f)) * s);
    }
    const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) -
                       (p[2].x - p[0].x) * (p[1].y - p[0].y);
    if (std::fabs(area) < 1e-9f) return;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
    const int x1 = std::min(t.size - 1, static_cast<int>(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
    const int y1 = std::min(t.size - 1, static_cast<int>(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
    if (x0 > x1 || y0 > y1) return;
    const float inv_area = 1.0f / area;
    const glm::vec3 light = glm::normalize(-view.forward + view.up * 0.6f);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const glm::vec2 c(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
            float w0 = ((p[1].x - c.x) * (p[2].y - c.y) - (p[2].x - c.x) * (p[1].y - c.y)) * inv_area;
            float w1 = ((p[2].x - c.x) * (p[0].y - c.y) - (p[0].x - c.x) * (p[2].y - c.y)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
            const float z = w0 * ndc[0].z + w1 * ndc[1].z + w2 * ndc[2].z;
            const size_t idx = static_cast<size_t>(y) * t.size + x;
            if (z < 0.0f || z >= t.depth[idx]) continue;
            // Perspective-correct attribute weights.
            float pw0 = w0 * inv_w[0], pw1 = w1 * inv_w[1], pw2 = w2 * inv_w[2];
            const float sum = pw0 + pw1 + pw2;
            pw0 /= sum;
            pw1 /= sum;
            pw2 /= sum;
            t.depth[idx] = z;
            const glm::vec3 wp = v[0].world * pw0 + v[1].world * pw1 + v[2].world * pw2;
            t.view_depth[idx] = glm::dot(wp - view.eye, view.forward);
            if (write_id) t.ids[idx] = id;
            glm::vec3 col;
            if (flat != nullptr) {
                col = *flat;
            } else {
                glm::vec4 base = v[0].color * pw0 + v[1].color * pw1 + v[2].color * pw2;
                if (tex != nullptr) {
                    const glm::vec2 uv = v[0].uv * pw0 + v[1].uv * pw1 + v[2].uv * pw2;
                    base *= tex->sample(uv.x, uv.y);
                }
                glm::vec3 n = v[0].normal * pw0 + v[1].normal * pw1 + v[2].normal * pw2;
                const float nl = glm::length(n);
                n = nl > 1e-6f ? n / nl : -view.forward;
                const float lambert = std::fabs(glm::dot(n, light));
                col = glm::vec3(base) * (0.38f + 0.72f * lambert);
            }
            col = glm::clamp(col, 0.0f, 1.0f);
            t.rgb[idx * 3 + 0] = static_cast<unsigned char>(col.r * 255.0f + 0.5f);
            t.rgb[idx * 3 + 1] = static_cast<unsigned char>(col.g * 255.0f + 0.5f);
            t.rgb[idx * 3 + 2] = static_cast<unsigned char>(col.b * 255.0f + 0.5f);
        }
    }
}

inline ClipVert make_vert(const glm::mat4& vp, const glm::vec3& pos, const glm::vec3& n,
                          const glm::vec2& uv, const glm::vec4& color) {
    ClipVert c;
    c.clip = vp * glm::vec4(pos, 1.0f);
    c.world = pos;
    c.normal = n;
    c.uv = uv;
    c.color = color;
    return c;
}

inline ClipVert lerp_vert(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert c;
    c.clip = glm::mix(a.clip, b.clip, t);
    c.world = glm::mix(a.world, b.world, t);
    c.normal = glm::mix(a.normal, b.normal, t);
    c.uv = glm::mix(a.uv, b.uv, t);
    c.color = glm::mix(a.color, b.color, t);
    return c;
}

/** ``raster_triangle`` after clipping against the near plane (clip z >= 0,
 *  the zero-to-one depth convention): a camera standing on a floor sees the
 *  floor, rather than losing every triangle that reaches behind it. */
inline void raster_clipped(Target& t, const Camera& view, const ClipVert v[3],
                           const Image* tex, uint32_t id, bool write_id,
                           const glm::vec3* flat) {
    const float eps = 1e-5f;
    int inside = 0;
    for (int i = 0; i < 3; ++i) inside += v[i].clip.z >= eps ? 1 : 0;
    if (inside == 3) {
        raster_triangle(t, view, v, tex, id, write_id, flat);
        return;
    }
    if (inside == 0) return;
    ClipVert poly[4];
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipVert& a = v[i];
        const ClipVert& b = v[(i + 1) % 3];
        const bool ain = a.clip.z >= eps, bin = b.clip.z >= eps;
        if (ain) poly[n++] = a;
        if (ain != bin) {
            const float s = (eps - a.clip.z) / (b.clip.z - a.clip.z);
            poly[n++] = lerp_vert(a, b, s);
        }
    }
    for (int k = 1; k + 1 < n; ++k) {
        const ClipVert tri[3] = {poly[0], poly[k], poly[k + 1]};
        raster_triangle(t, view, tri, tex, id, write_id, flat);
    }
}

}  // namespace ptv_raster
