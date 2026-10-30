// ptv_glb_views: offline multi-view renderer for socket generation.
//
// The socket_generation stage asks a vision model to find a pistol's handle,
// a shield's straps or a sword's blade in pictures of the mesh, then maps
// what it found back onto the mesh. That mapping is only exact if, for every
// pixel of every picture, we know WHICH TRIANGLE it shows. So this tool does
// not just take screenshots: next to each colour image it writes
//
//   view_<k>.ids    uint32 per pixel: triangle index + 1 (0 = background)
//   view_<k>.depth  float32 per pixel: distance along the camera's forward
//
// plus ``cameras.json`` (view / projection matrices of every view) and
// ``mesh.bin`` (the merged positions + indices the triangle ids refer to).
// A mask pixel is then a triangle id lookup -- no raycasting, no camera
// reconstruction error -- and a keypoint is a surface point to the
// sub-pixel.
//
// Geometry comes from ``load_glb_textured``, which shares the runtime's node
// traversal with ``load_glb_merged``: triangle i here is triangle i in the
// mesh the runtime draws and that socket positions are expressed in.
//
// Views live in the mesh's PCA frame so a pistol is photographed side-on no
// matter how the generator rotated it: six orthographic views along the
// principal axes and four three-quarter perspective views. A CPU rasteriser
// (z-buffer, textured Lambert) keeps it deterministic and GPU-free.
//
// ``--overlay <markers.json>`` re-renders the same views with marker
// geometry (a proxy fist, arrows, bars, spheres) composited in depth, for the
// render-and-verify pass and for human review.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "ptv/render/GltfMeshLoad.h"

namespace {

namespace fs = std::filesystem;
using ptv::gltf_mesh_load::TexturedMesh;

struct Options {
    std::string input;
    std::string out_dir;
    std::string overlay;
    int size = 768;
    // Optional world "up" for the views (mesh space): verification renders
    // pass the item's solved up so a pistol is photographed upright.
    bool has_up = false;
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    bool has_forward = false;
    glm::vec3 forward{0.0f, 0.0f, 1.0f};
};

bool parse_vec3(const std::string& s, glm::vec3& out) {
    float x = 0, y = 0, z = 0;
    if (std::sscanf(s.c_str(), "%f,%f,%f", &x, &y, &z) != 3) return false;
    out = glm::vec3(x, y, z);
    return glm::length(out) > 1e-6f;
}

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
        } else if (a == "--out-dir") {
            if (!next(o.out_dir)) return false;
        } else if (a == "--overlay") {
            if (!next(o.overlay)) return false;
        } else if (a == "--size") {
            if (!next(v)) return false;
            o.size = std::clamp(std::stoi(v), 128, 2048);
        } else if (a == "--up") {
            if (!next(v) || !parse_vec3(v, o.up)) return false;
            o.has_up = true;
        } else if (a == "--forward") {
            if (!next(v) || !parse_vec3(v, o.forward)) return false;
            o.has_forward = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return !o.input.empty() && !o.out_dir.empty();
}

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

// --- PCA (cyclic Jacobi, as in glb_sockets) ---------------------------------
struct Eigen3 {
    glm::vec3 values{0.0f};
    glm::mat3 vectors{1.0f};
};

Eigen3 jacobi_eigen(glm::mat3 a) {
    glm::mat3 v(1.0f);
    for (int sweep = 0; sweep < 24; ++sweep) {
        float off = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j) off += a[j][i] * a[j][i];
        if (off < 1e-20f) break;
        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const float apq = a[q][p];
                if (std::fabs(apq) < 1e-20f) continue;
                const float theta = (a[q][q] - a[p][p]) / (2.0f * apq);
                const float t = (theta >= 0.0f ? 1.0f : -1.0f) /
                                (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
                const float c = 1.0f / std::sqrt(t * t + 1.0f);
                const float s = t * c;
                glm::mat3 rot(1.0f);
                rot[p][p] = c;
                rot[q][q] = c;
                rot[q][p] = s;
                rot[p][q] = -s;
                a = glm::transpose(rot) * a * rot;
                v = v * rot;
            }
        }
    }
    Eigen3 out;
    int order[3] = {0, 1, 2};
    const float d[3] = {a[0][0], a[1][1], a[2][2]};
    std::sort(order, order + 3,
              [&](int l, int r) { return std::fabs(d[l]) > std::fabs(d[r]); });
    for (int i = 0; i < 3; ++i) {
        out.values[i] = d[order[i]];
        out.vectors[i] = glm::normalize(v[order[i]]);
    }
    return out;
}

// --- Views -------------------------------------------------------------------
struct View {
    std::string name;
    bool ortho = true;
    glm::vec3 eye{0.0f}, forward{0.0f}, up{0.0f};
    glm::mat4 view{1.0f}, proj{1.0f};
};

std::vector<View> make_views(const glm::vec3& center, float radius,
                             const glm::vec3 axes[3]) {
    std::vector<View> out;
    const float margin = 1.12f;
    auto add = [&](const std::string& name, glm::vec3 dir, glm::vec3 up, bool ortho) {
        dir = glm::normalize(dir);
        // Keep "up" perpendicular to the view direction.
        up = glm::normalize(up - dir * glm::dot(up, dir));
        View v;
        v.name = name;
        v.ortho = ortho;
        v.forward = -dir;
        v.up = up;
        if (ortho) {
            v.eye = center + dir * (radius * 4.0f);
            v.proj = glm::orthoRH_ZO(-radius * margin, radius * margin,
                                     -radius * margin, radius * margin,
                                     radius * 0.5f, radius * 8.0f);
        } else {
            const float fov = glm::radians(35.0f);
            const float dist = radius * margin / std::sin(fov * 0.5f);
            v.eye = center + dir * dist;
            v.proj = glm::perspectiveRH_ZO(fov, 1.0f, std::max(1e-4f, dist - radius * 2.0f),
                                           dist + radius * 2.0f);
        }
        v.view = glm::lookAtRH(v.eye, center, up);
        out.push_back(v);
    };
    const glm::vec3 e0 = axes[0], e1 = axes[1], e2 = axes[2];
    // Side views (the silhouette that shows a handle, a blade, a bow's limbs).
    add("side_a", e2, e1, true);
    add("side_b", -e2, e1, true);
    add("top", e1, e0, true);
    add("bottom", -e1, e0, true);
    add("end_a", e0, e1, true);
    add("end_b", -e0, e1, true);
    add("three_quarter_a", e2 + e1 * 0.6f + e0 * 0.6f, e1, false);
    add("three_quarter_b", e2 + e1 * 0.6f - e0 * 0.6f, e1, false);
    add("three_quarter_c", -e2 + e1 * 0.6f + e0 * 0.6f, e1, false);
    add("three_quarter_d", -e2 - e1 * 0.6f - e0 * 0.6f, e1, false);
    return out;
}

// --- Rasteriser --------------------------------------------------------------
struct Target {
    int size = 0;
    std::vector<float> depth;          // NDC z (0 near, 1 far)
    std::vector<float> view_depth;     // distance along forward
    std::vector<uint32_t> ids;         // triangle + 1
    std::vector<unsigned char> rgb;
    explicit Target(int s)
        : size(s),
          depth(static_cast<size_t>(s) * s, 1.0f),
          view_depth(static_cast<size_t>(s) * s, 0.0f),
          ids(static_cast<size_t>(s) * s, 0u),
          rgb(static_cast<size_t>(s) * s * 3, 0) {
        for (int i = 0; i < s * s; ++i) {
            // Neutral light grey: no colour a material could be confused with.
            rgb[i * 3 + 0] = 196;
            rgb[i * 3 + 1] = 198;
            rgb[i * 3 + 2] = 202;
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
void raster_triangle(Target& t, const View& view, const ClipVert v[3],
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

ClipVert make_vert(const glm::mat4& vp, const glm::vec3& pos, const glm::vec3& n,
                   const glm::vec2& uv, const glm::vec4& color) {
    ClipVert c;
    c.clip = vp * glm::vec4(pos, 1.0f);
    c.world = pos;
    c.normal = n;
    c.uv = uv;
    c.color = color;
    return c;
}

// --- Marker geometry (overlay mode) ------------------------------------------
struct Tri {
    glm::vec3 a, b, c;
    glm::vec3 color;
};

void add_cylinder(std::vector<Tri>& out, glm::vec3 a, glm::vec3 b, float r,
                  glm::vec3 color, int seg = 18) {
    glm::vec3 axis = b - a;
    const float len = glm::length(axis);
    if (len < 1e-6f) return;
    axis /= len;
    glm::vec3 u = std::fabs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(axis, u));
    const glm::vec3 w = glm::cross(axis, u);
    for (int i = 0; i < seg; ++i) {
        const float t0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
        const float t1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(seg);
        const glm::vec3 d0 = (u * std::cos(t0) + w * std::sin(t0)) * r;
        const glm::vec3 d1 = (u * std::cos(t1) + w * std::sin(t1)) * r;
        out.push_back({a + d0, b + d0, b + d1, color});
        out.push_back({a + d0, b + d1, a + d1, color});
        out.push_back({a, a + d0, a + d1, color});
        out.push_back({b, b + d1, b + d0, color});
    }
}

void add_cone(std::vector<Tri>& out, glm::vec3 base, glm::vec3 tip, float r,
              glm::vec3 color, int seg = 18) {
    glm::vec3 axis = tip - base;
    const float len = glm::length(axis);
    if (len < 1e-6f) return;
    axis /= len;
    glm::vec3 u = std::fabs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(axis, u));
    const glm::vec3 w = glm::cross(axis, u);
    for (int i = 0; i < seg; ++i) {
        const float t0 = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
        const float t1 = 6.2831853f * static_cast<float>(i + 1) / static_cast<float>(seg);
        const glm::vec3 d0 = (u * std::cos(t0) + w * std::sin(t0)) * r;
        const glm::vec3 d1 = (u * std::cos(t1) + w * std::sin(t1)) * r;
        out.push_back({base + d0, tip, base + d1, color});
        out.push_back({base, base + d1, base + d0, color});
    }
}

void add_sphere(std::vector<Tri>& out, glm::vec3 c, float r, glm::vec3 color) {
    const int rings = 10, seg = 16;
    auto pt = [&](int i, int j) {
        const float th = 3.14159265f * static_cast<float>(i) / rings;
        const float ph = 6.2831853f * static_cast<float>(j) / seg;
        return c + r * glm::vec3(std::sin(th) * std::cos(ph), std::cos(th),
                                 std::sin(th) * std::sin(ph));
    };
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < seg; ++j) {
            out.push_back({pt(i, j), pt(i + 1, j), pt(i + 1, j + 1), color});
            out.push_back({pt(i, j), pt(i + 1, j + 1), pt(i, j + 1), color});
        }
    }
}

glm::vec3 jvec3(const nlohmann::json& j, glm::vec3 def = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return def;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

std::vector<Tri> load_markers(const std::string& path) {
    std::vector<Tri> out;
    std::ifstream in(path);
    if (!in) return out;
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_object() || !doc.contains("markers")) return out;
    for (const auto& m : doc["markers"]) {
        const std::string type = m.value("type", "");
        const glm::vec3 color = jvec3(m.value("color", nlohmann::json()), glm::vec3(1, 0, 1));
        if (type == "sphere") {
            add_sphere(out, jvec3(m["position"]), m.value("radius", 0.01f), color);
        } else if (type == "arrow") {
            const glm::vec3 a = jvec3(m["position"]);
            const glm::vec3 d = glm::normalize(jvec3(m["direction"], glm::vec3(0, 0, 1)));
            const float len = m.value("length", 0.1f);
            const float r = m.value("radius", len * 0.04f);
            add_cylinder(out, a, a + d * len * 0.78f, r, color);
            add_cone(out, a + d * len * 0.78f, a + d * len, r * 2.6f, color);
        } else if (type == "bar") {
            add_cylinder(out, jvec3(m["a"]), jvec3(m["b"]), m.value("radius", 0.005f), color);
        } else if (type == "fist") {
            // A band around the gripped part: the palm + closed fingers,
            // with its axis along the socket's -Z (little finger -> thumb).
            const glm::vec3 c = jvec3(m["position"]);
            const auto& q = m["rotation"];
            const glm::quat rot(q[3].get<float>(), q[0].get<float>(), q[1].get<float>(),
                                q[2].get<float>());
            const float width = m.value("width", 0.09f);
            const float radius = m.value("radius", 0.03f);
            const glm::vec3 z = rot * glm::vec3(0, 0, 1);
            const glm::vec3 y = rot * glm::vec3(0, 1, 0);
            add_cylinder(out, c + z * (width * 0.5f), c - z * (width * 0.5f), radius,
                         color, 24);
            // Arrows long and thick enough to read past the band on a thin
            // hilt: they start at the band's surface, not inside it.
            const float len = std::max(width * 1.1f, radius * 2.5f);
            const float thick = std::max(radius * 0.22f, width * 0.06f);
            const glm::vec3 cyan(0.1f, 0.9f, 1.0f), magenta(1.0f, 0.2f, 0.9f);
            const glm::vec3 z0 = c - z * (width * 0.5f);
            add_cylinder(out, z0, z0 - z * len, thick, cyan);
            add_cone(out, z0 - z * len, z0 - z * (len * 1.3f), thick * 2.4f, cyan);
            const glm::vec3 y0 = c - y * radius;
            add_cylinder(out, y0, y0 - y * len, thick, magenta);
            add_cone(out, y0 - y * len, y0 - y * (len * 1.3f), thick * 2.4f, magenta);
        }
    }
    return out;
}

nlohmann::json mat_json(const glm::mat4& m) {
    nlohmann::json a = nlohmann::json::array();
    const float* p = glm::value_ptr(m);
    for (int i = 0; i < 16; ++i) a.push_back(p[i]);
    return a;
}

nlohmann::json vec_json(const glm::vec3& v) { return {v.x, v.y, v.z}; }

bool write_raw(const fs::path& path, const void* data, size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        std::fprintf(stderr,
                     "usage: ptv_glb_views --input <in.glb> --out-dir <dir> "
                     "[--size 768] [--overlay markers.json]\n");
        return 2;
    }
    TexturedMesh mesh;
    std::string err;
    if (!ptv::gltf_mesh_load::load_glb_textured(opt.input, mesh, err)) {
        std::fprintf(stderr, "load failed (%s): %s\n", opt.input.c_str(), err.c_str());
        return 1;
    }
    std::vector<Image> images(mesh.images.size());
    for (size_t i = 0; i < mesh.images.size(); ++i) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(
            mesh.images[i].bytes.data(), static_cast<int>(mesh.images[i].bytes.size()),
            &w, &h, &n, 4);
        if (px != nullptr) {
            images[i].w = w;
            images[i].h = h;
            images[i].rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
        }
    }

    // Bounds, centroid, PCA.
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    glm::dvec3 sum(0.0);
    for (const auto& v : mesh.verts) {
        const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        sum += glm::dvec3(p);
    }
    const glm::vec3 centroid(sum / static_cast<double>(mesh.verts.size()));
    const glm::vec3 center = (lo + hi) * 0.5f;
    float radius = 0.0f;
    for (const auto& v : mesh.verts) {
        radius = std::max(radius, glm::length(glm::vec3(v.pos[0], v.pos[1], v.pos[2]) - center));
    }
    if (radius < 1e-6f) {
        std::fprintf(stderr, "degenerate mesh\n");
        return 1;
    }
    glm::mat3 cov(0.0f);
    for (const auto& v : mesh.verts) {
        const glm::vec3 d = glm::vec3(v.pos[0], v.pos[1], v.pos[2]) - centroid;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) cov[c][r] += d[r] * d[c];
    }
    cov /= static_cast<float>(mesh.verts.size());
    const Eigen3 eig = jacobi_eigen(cov);
    glm::vec3 axes[3] = {eig.vectors[0], eig.vectors[1], eig.vectors[2]};
    // Deterministic signs: each axis points toward the side of the mesh's
    // bounding-box centre that its larger half lies on (then right-handed).
    for (int i = 0; i < 2; ++i) {
        if (glm::dot(center - centroid, axes[i]) < 0.0f) axes[i] = -axes[i];
    }
    axes[2] = glm::normalize(glm::cross(axes[0], axes[1]));
    // A known up / forward (from a previous solve) replaces the PCA frame so
    // the item is photographed upright, facing a consistent way.
    if (opt.has_up) {
        const glm::vec3 up = glm::normalize(opt.up);
        glm::vec3 fwd = opt.has_forward ? opt.forward : axes[0];
        fwd = fwd - up * glm::dot(fwd, up);
        if (glm::length(fwd) < 1e-4f) fwd = axes[2] - up * glm::dot(axes[2], up);
        axes[0] = glm::normalize(fwd);
        axes[1] = up;
        axes[2] = glm::normalize(glm::cross(axes[0], axes[1]));
    }

    // Vertex normals when the GLB has none.
    bool need_normals = false;
    for (const auto& v : mesh.verts) {
        if (v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2] < 1e-8f) {
            need_normals = true;
            break;
        }
    }
    std::vector<glm::vec3> normals(mesh.verts.size(), glm::vec3(0.0f));
    for (size_t i = 0; i < mesh.verts.size(); ++i)
        normals[i] = glm::vec3(mesh.verts[i].normal[0], mesh.verts[i].normal[1],
                               mesh.verts[i].normal[2]);
    if (need_normals) {
        std::fill(normals.begin(), normals.end(), glm::vec3(0.0f));
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const auto& a = mesh.verts[mesh.indices[t]].pos;
            const auto& b = mesh.verts[mesh.indices[t + 1]].pos;
            const auto& c = mesh.verts[mesh.indices[t + 2]].pos;
            const glm::vec3 n = glm::cross(glm::vec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]),
                                           glm::vec3(c[0] - a[0], c[1] - a[1], c[2] - a[2]));
            for (int k = 0; k < 3; ++k) normals[mesh.indices[t + k]] += n;
        }
    }

    const std::vector<View> views = make_views(center, radius, axes);
    const std::vector<Tri> markers =
        opt.overlay.empty() ? std::vector<Tri>{} : load_markers(opt.overlay);
    const bool overlay = !opt.overlay.empty();

    fs::create_directories(opt.out_dir);
    nlohmann::json cams = nlohmann::json::array();
    const size_t tri_count = mesh.indices.size() / 3;
    for (size_t k = 0; k < views.size(); ++k) {
        const View& view = views[k];
        const glm::mat4 vp = view.proj * view.view;
        Target target(opt.size);
        for (size_t t = 0; t < tri_count; ++t) {
            ClipVert cv[3];
            for (int i = 0; i < 3; ++i) {
                const auto& tv = mesh.verts[mesh.indices[t * 3 + i]];
                cv[i] = make_vert(vp, glm::vec3(tv.pos[0], tv.pos[1], tv.pos[2]),
                                  normals[mesh.indices[t * 3 + i]],
                                  glm::vec2(tv.uv[0], tv.uv[1]),
                                  glm::vec4(tv.color[0], tv.color[1], tv.color[2], tv.color[3]));
            }
            const int img = t < mesh.tri_image.size() ? mesh.tri_image[t] : -1;
            const Image* tex = (img >= 0 && static_cast<size_t>(img) < images.size() &&
                                images[img].w > 0)
                                   ? &images[img]
                                   : nullptr;
            raster_triangle(target, view, cv, tex, static_cast<uint32_t>(t + 1), true, nullptr);
        }
        for (const auto& m : markers) {
            ClipVert cv[3] = {make_vert(vp, m.a, glm::vec3(0), glm::vec2(0), glm::vec4(1)),
                              make_vert(vp, m.b, glm::vec3(0), glm::vec2(0), glm::vec4(1)),
                              make_vert(vp, m.c, glm::vec3(0), glm::vec2(0), glm::vec4(1))};
            raster_triangle(target, view, cv, nullptr, 0, false, &m.color);
        }
        const std::string stem = (overlay ? "overlay_" : "view_") + std::to_string(k);
        const fs::path png = fs::path(opt.out_dir) / (stem + ".png");
        if (!stbi_write_png(png.string().c_str(), opt.size, opt.size, 3, target.rgb.data(),
                            opt.size * 3)) {
            std::fprintf(stderr, "cannot write %s\n", png.string().c_str());
            return 1;
        }
        if (!overlay) {
            write_raw(fs::path(opt.out_dir) / (stem + ".ids"), target.ids.data(),
                      target.ids.size() * sizeof(uint32_t));
            write_raw(fs::path(opt.out_dir) / (stem + ".depth"), target.view_depth.data(),
                      target.view_depth.size() * sizeof(float));
        }
        cams.push_back({{"index", k},
                        {"name", view.name},
                        {"ortho", view.ortho},
                        {"image", stem + ".png"},
                        {"width", opt.size},
                        {"height", opt.size},
                        {"eye", vec_json(view.eye)},
                        {"forward", vec_json(view.forward)},
                        {"up", vec_json(view.up)},
                        {"view", mat_json(view.view)},
                        {"proj", mat_json(view.proj)}});
    }

    if (!overlay) {
        // mesh.bin: u32 vertex count, u32 triangle count, f32 xyz * n, u32 idx * 3t.
        std::ofstream out(fs::path(opt.out_dir) / "mesh.bin", std::ios::binary);
        const uint32_t nv = static_cast<uint32_t>(mesh.verts.size());
        const uint32_t nt = static_cast<uint32_t>(tri_count);
        out.write(reinterpret_cast<const char*>(&nv), 4);
        out.write(reinterpret_cast<const char*>(&nt), 4);
        for (const auto& v : mesh.verts) out.write(reinterpret_cast<const char*>(v.pos), 12);
        out.write(reinterpret_cast<const char*>(mesh.indices.data()),
                  static_cast<std::streamsize>(mesh.indices.size() * 4));
    }
    nlohmann::json doc;
    doc["input"] = opt.input;
    doc["size"] = opt.size;
    doc["vertices"] = mesh.verts.size();
    doc["triangles"] = tri_count;
    doc["aabb_min"] = vec_json(lo);
    doc["aabb_max"] = vec_json(hi);
    doc["longest_axis"] = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    doc["center"] = vec_json(center);
    doc["centroid"] = vec_json(centroid);
    doc["radius"] = radius;
    doc["axes"] = {vec_json(axes[0]), vec_json(axes[1]), vec_json(axes[2])};
    doc["eigenvalues"] = vec_json(eig.values);
    doc["textured"] = !images.empty();
    doc["views"] = cams;
    std::ofstream cj(fs::path(opt.out_dir) / (overlay ? "overlay_cameras.json" : "cameras.json"));
    cj << doc.dump(2) << "\n";
    return 0;
}
