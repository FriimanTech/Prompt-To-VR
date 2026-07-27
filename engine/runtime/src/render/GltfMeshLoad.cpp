// GLB loader for the VR runtime.
//
// We merge all triangle primitives into one indexed triangle list, rescale
// to ``target_scale_meters`` on the longest axis of the model AABB, and
// fill ``pos`` + ``normal`` + ``color`` in the same layout as the renderer's
// vertex struct.
//
// Image data is intentionally NOT decoded into GPU textures here -- the
// engine uses a single-pipeline forward shader without sampler descriptors
// today. Instead we read each primitive's material ``baseColorFactor``
// (and ``COLOR_0`` vertex stream when present) and bake it into the
// per-vertex color channel, so every Tripo / generated GLB renders with
// its actual design color rather than a uniform tint.
//
// IMPORTANT: When ``TINYGLTF_NO_STB_IMAGE`` is set, tinygltf REQUIRES a
// LoadImageData callback registered via ``TinyGLTF::SetImageLoader``,
// otherwise ``LoadBinaryFromFile`` aborts with
// "No LoadImageData callback specified.". We register a no-op callback
// (``noop_image_loader`` below) that simply succeeds without decoding so
// the rest of the GLB (geometry + materials) parses cleanly.
//   Reference: tinygltf README, "Optional Compile flags" section.

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/GltfMeshLoad.h"

namespace ptv::gltf_mesh_load {
namespace {

// No-op image loader: tinygltf calls this once per ``image`` in the GLB.
// We don't sample textures in the renderer yet, so we simply tell tinygltf
// "yes, the image is fine" without decoding any pixels. This is the
// pattern recommended by tinygltf when running with ``TINYGLTF_NO_STB_IMAGE``.
//
// Returning true (without populating ``image->image``) means the rest of
// the GLB parses normally. Material lookups (baseColorTexture indices,
// baseColorFactor, etc.) still resolve and we use them below.
bool noop_image_loader(tinygltf::Image* image,
                       const int /*image_idx*/,
                       std::string* /*err*/,
                       std::string* /*warn*/,
                       int /*req_width*/,
                       int /*req_height*/,
                       const unsigned char* /*bytes*/,
                       int /*size*/,
                       void* /*user_data*/) {
    if (image) {
        image->width = 0;
        image->height = 0;
        image->component = 0;
        image->bits = 8;
        image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        image->image.clear();
    }
    return true;
}

size_t component_size(int component_type) {
    switch (component_type) {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return 1;
        case TINYGLTF_COMPONENT_TYPE_SHORT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            return 2;
        case TINYGLTF_COMPONENT_TYPE_INT:
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            return 4;
        default:
            return 0;
    }
}

size_t num_components(int type) {
    switch (type) {
        case TINYGLTF_TYPE_SCALAR:
            return 1;
        case TINYGLTF_TYPE_VEC2:
            return 2;
        case TINYGLTF_TYPE_VEC3:
            return 3;
        case TINYGLTF_TYPE_VEC4:
            return 4;
        default:
            return 0;
    }
}

bool read_accessor_float3(const tinygltf::Model& model,
                            int accessor_idx,
                            std::vector<std::array<float, 3>>& out) {
    if (accessor_idx < 0 ||
        static_cast<size_t>(accessor_idx) >= model.accessors.size()) {
        return false;
    }
    const auto& acc = model.accessors[accessor_idx];
    if (acc.type != TINYGLTF_TYPE_VEC3 ||
        acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) {
        return false;
    }
    if (acc.bufferView < 0 ||
        static_cast<size_t>(acc.bufferView) >= model.bufferViews.size()) {
        return false;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    if (view.buffer < 0 ||
        static_cast<size_t>(view.buffer) >= model.buffers.size()) {
        return false;
    }
    const auto& buf = model.buffers[view.buffer];
    size_t stride = view.byteStride;
    if (stride == 0) stride = sizeof(float) * 3;
    const size_t base = static_cast<size_t>(acc.byteOffset + view.byteOffset);
    if (base + acc.count * stride > buf.data.size()) return false;

    out.resize(acc.count);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = buf.data.data() + base + i * stride;
        std::memcpy(out[i].data(), p, sizeof(float) * 3);
    }
    return true;
}

// Read a per-vertex VEC3 / VEC4 color stream (``COLOR_0``) into RGBA float.
// Tripo and most authoring tools emit either VEC3_FLOAT or VEC4_FLOAT;
// some pack as UNSIGNED_BYTE / UNSIGNED_SHORT normalized. We handle the
// common cases and silently skip anything we don't recognise (the caller
// then falls back to baseColorFactor).
bool read_accessor_color_rgba(const tinygltf::Model& model,
                              int accessor_idx,
                              std::vector<std::array<float, 4>>& out) {
    if (accessor_idx < 0 ||
        static_cast<size_t>(accessor_idx) >= model.accessors.size()) {
        return false;
    }
    const auto& acc = model.accessors[accessor_idx];
    const size_t nc = num_components(acc.type);
    if (nc != 3 && nc != 4) return false;
    if (acc.bufferView < 0 ||
        static_cast<size_t>(acc.bufferView) >= model.bufferViews.size()) {
        return false;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    if (view.buffer < 0 ||
        static_cast<size_t>(view.buffer) >= model.buffers.size()) {
        return false;
    }
    const auto& buf = model.buffers[view.buffer];
    const size_t cs = component_size(acc.componentType);
    if (cs == 0) return false;
    size_t stride = view.byteStride;
    if (stride == 0) stride = cs * nc;
    const size_t base =
        static_cast<size_t>(acc.byteOffset + view.byteOffset);
    if (base + acc.count * stride > buf.data.size()) return false;

    out.resize(acc.count);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = buf.data.data() + base + i * stride;
        float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        for (size_t k = 0; k < nc; ++k) {
            float v = 1.0f;
            const uint8_t* q = p + k * cs;
            switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_FLOAT: {
                    float f;
                    std::memcpy(&f, q, sizeof(f));
                    v = f;
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    v = static_cast<float>(*q) / 255.0f;
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    uint16_t u;
                    std::memcpy(&u, q, sizeof(u));
                    v = static_cast<float>(u) / 65535.0f;
                    break;
                }
                default:
                    return false;
            }
            c[k] = v;
        }
        out[i] = {c[0], c[1], c[2], c[3]};
    }
    return true;
}

bool read_accessor_indices(const tinygltf::Model& model,
                           int accessor_idx,
                           std::vector<uint32_t>& out) {
    if (accessor_idx < 0 ||
        static_cast<size_t>(accessor_idx) >= model.accessors.size()) {
        return false;
    }
    const auto& acc = model.accessors[accessor_idx];
    if (acc.type != TINYGLTF_TYPE_SCALAR) return false;
    if (acc.bufferView < 0 ||
        static_cast<size_t>(acc.bufferView) >= model.bufferViews.size()) {
        return false;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    if (view.buffer < 0 ||
        static_cast<size_t>(view.buffer) >= model.buffers.size()) {
        return false;
    }
    const auto& buf = model.buffers[view.buffer];
    size_t cs = component_size(acc.componentType);
    size_t nc = num_components(acc.type);
    if (cs == 0 || nc != 1) return false;
    size_t stride = view.byteStride;
    if (stride == 0) stride = cs;
    const size_t base = static_cast<size_t>(acc.byteOffset + view.byteOffset);
    if (base + acc.count * stride > buf.data.size()) return false;

    out.resize(acc.count);
    for (size_t i = 0; i < acc.count; ++i) {
        const uint8_t* p = buf.data.data() + base + i * stride;
        uint32_t v = 0;
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
            uint16_t u;
            std::memcpy(&u, p, sizeof(u));
            v = u;
        } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
            std::memcpy(&v, p, sizeof(v));
        } else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
            v = *p;
        } else {
            return false;
        }
        out[i] = v;
    }
    return true;
}

void compute_vertex_normals(const std::vector<uint32_t>& indices,
                            std::vector<LitVertex>& verts) {
    for (auto& v : verts) {
        v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t ia = indices[i];
        uint32_t ib = indices[i + 1];
        uint32_t ic = indices[i + 2];
        if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size())
            continue;
        float* a = verts[ia].pos;
        float* b = verts[ib].pos;
        float* c = verts[ic].pos;
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float nx = e1[1] * e2[2] - e1[2] * e2[1];
        float ny = e1[2] * e2[0] - e1[0] * e2[2];
        float nz = e1[0] * e2[1] - e1[1] * e2[0];
        for (uint32_t id : {ia, ib, ic}) {
            verts[id].normal[0] += nx;
            verts[id].normal[1] += ny;
            verts[id].normal[2] += nz;
        }
    }
    for (auto& v : verts) {
        float len = std::sqrt(v.normal[0] * v.normal[0] +
                              v.normal[1] * v.normal[1] +
                              v.normal[2] * v.normal[2]);
        if (len > 1e-8f) {
            v.normal[0] /= len;
            v.normal[1] /= len;
            v.normal[2] /= len;
        } else {
            v.normal[0] = 0.0f;
            v.normal[1] = 1.0f;
            v.normal[2] = 0.0f;
        }
    }
}

void uniform_scale_positions(std::vector<LitVertex>& verts, float s) {
    for (auto& v : verts) {
        v.pos[0] *= s;
        v.pos[1] *= s;
        v.pos[2] *= s;
    }
}

void bounds_max_extent(const std::vector<LitVertex>& verts,
                       float* min_out,
                       float* max_out) {
    float min_b[3] = {1e30f, 1e30f, 1e30f};
    float max_b[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& v : verts) {
        for (int k = 0; k < 3; ++k) {
            min_b[k] = std::min(min_b[k], v.pos[k]);
            max_b[k] = std::max(max_b[k], v.pos[k]);
        }
    }
    float ext_x = max_b[0] - min_b[0];
    float ext_y = max_b[1] - min_b[1];
    float ext_z = max_b[2] - min_b[2];
    *max_out = std::max(ext_x, std::max(ext_y, ext_z));
    if (min_out) {
        *min_out = std::min(ext_x, std::min(ext_y, ext_z));
    }
}

}  // namespace

bool load_glb_merged(const std::string& path,
                     float target_scale_meters,
                     std::vector<LitVertex>& out_verts,
                     std::vector<uint32_t>& out_indices,
                     std::string& err) {
    out_verts.clear();
    out_indices.clear();
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    // tinygltf is built without stb_image (TINYGLTF_NO_STB_IMAGE), so it
    // would otherwise abort with "No LoadImageData callback specified".
    // Install the no-op loader before we touch the file -- materials are
    // still parsed, we just skip pixel decoding.
    loader.SetImageLoader(noop_image_loader, nullptr);
    std::string warn;
    // Read the GLB bytes through the process-global asset source (filesystem
    // on desktop, APK/AAssetManager on Quest) and parse from memory, so the
    // loader is not tied to a filesystem path that does not exist on-device.
    auto bytes = asset_source().read_bytes(path);
    if (!bytes) {
        err = "asset source could not read GLB: " + path;
        return false;
    }
    const std::string base_dir = std::filesystem::path(path).parent_path().string();
    if (!loader.LoadBinaryFromMemory(&model, &err, &warn, bytes->data(),
                                     static_cast<unsigned int>(bytes->size()),
                                     base_dir)) {
        if (warn.size()) PTV_WARN("tinygltf warn: {}", warn);
        return false;
    }
    if (warn.size()) PTV_WARN("tinygltf warn: {}", warn);

    // Per-primitive material color lookup. Each merged primitive picks up
    // its own ``baseColorFactor`` from the glTF material and bakes it
    // into every vertex it contributes. This is what gives Tripo's
    // ``central_battery_tower`` its lantern-green glow, the
    // ``oa_skybox_stars`` its dark blue, etc., even though we don't yet
    // sample the baseColorTexture itself.
    auto material_base_color = [&](int material_idx) -> std::array<float, 4> {
        std::array<float, 4> c{1.0f, 1.0f, 1.0f, 1.0f};
        if (material_idx < 0 ||
            static_cast<size_t>(material_idx) >= model.materials.size()) {
            return c;
        }
        const auto& mat = model.materials[material_idx];
        const auto& bcf = mat.pbrMetallicRoughness.baseColorFactor;
        if (bcf.size() >= 3) {
            c[0] = static_cast<float>(bcf[0]);
            c[1] = static_cast<float>(bcf[1]);
            c[2] = static_cast<float>(bcf[2]);
            c[3] = (bcf.size() >= 4) ? static_cast<float>(bcf[3]) : 1.0f;
        }
        return c;
    };

    for (const auto& mesh : model.meshes) {
        for (const auto& prim : mesh.primitives) {
            // mode == -1 means triangles in glTF 2.0 default.
            if (!(prim.mode == TINYGLTF_MODE_TRIANGLES || prim.mode == -1)) {
                continue;
            }
            auto pos_it = prim.attributes.find("POSITION");
            if (pos_it == prim.attributes.end()) continue;

            std::vector<std::array<float, 3>> positions;
            if (!read_accessor_float3(model, pos_it->second, positions)) {
                continue;
            }

            std::vector<std::array<float, 3>> normals;
            auto n_it = prim.attributes.find("NORMAL");
            if (n_it != prim.attributes.end()) {
                read_accessor_float3(model, n_it->second, normals);
            }

            // Optional per-vertex COLOR_0 stream. When present, glTF spec
            // says we must multiply by baseColorFactor.
            std::vector<std::array<float, 4>> vert_colors;
            auto c_it = prim.attributes.find("COLOR_0");
            if (c_it != prim.attributes.end()) {
                read_accessor_color_rgba(model, c_it->second, vert_colors);
            }

            const std::array<float, 4> mat_color =
                material_base_color(prim.material);

            const uint32_t base =
                static_cast<uint32_t>(out_verts.size());
            for (size_t i = 0; i < positions.size(); ++i) {
                LitVertex lv{};
                lv.pos[0] = positions[i][0];
                lv.pos[1] = positions[i][1];
                lv.pos[2] = positions[i][2];
                if (i < normals.size()) {
                    lv.normal[0] = normals[i][0];
                    lv.normal[1] = normals[i][1];
                    lv.normal[2] = normals[i][2];
                } else {
                    lv.normal[0] = 0.0f;
                    lv.normal[1] = 1.0f;
                    lv.normal[2] = 0.0f;
                }
                if (i < vert_colors.size()) {
                    lv.color[0] = vert_colors[i][0] * mat_color[0];
                    lv.color[1] = vert_colors[i][1] * mat_color[1];
                    lv.color[2] = vert_colors[i][2] * mat_color[2];
                    lv.color[3] = vert_colors[i][3] * mat_color[3];
                } else {
                    lv.color[0] = mat_color[0];
                    lv.color[1] = mat_color[1];
                    lv.color[2] = mat_color[2];
                    lv.color[3] = mat_color[3];
                }
                out_verts.push_back(lv);
            }

            if (prim.indices >= 0) {
                std::vector<uint32_t> idx;
                if (!read_accessor_indices(model, prim.indices, idx)) {
                    // Drop vertices we added for this primitive if indices bad
                    out_verts.resize(base);
                    continue;
                }
                for (uint32_t id : idx) {
                    out_indices.push_back(base + id);
                }
            } else {
                // Non-indexed triangles
                if (positions.size() % 3 != 0) {
                    out_verts.resize(base);
                    continue;
                }
                for (size_t i = 0; i < positions.size(); i += 3) {
                    out_indices.push_back(base + static_cast<uint32_t>(i));
                    out_indices.push_back(base + static_cast<uint32_t>(i + 1));
                    out_indices.push_back(base + static_cast<uint32_t>(i + 2));
                }
            }
        }
    }

    if (out_verts.empty() || out_indices.empty()) {
        err = "no triangle geometry in GLB";
        return false;
    }

    bool need_normals = false;
    for (const auto& v : out_verts) {
        float nlen = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                               v.normal[2] * v.normal[2]);
        if (nlen < 1e-4f) {
            need_normals = true;
            break;
        }
    }
    if (need_normals) {
        compute_vertex_normals(out_indices, out_verts);
    }

    float min_e_unused = 0.0f;
    float max_e = 0.0f;
    bounds_max_extent(out_verts, &min_e_unused, &max_e);
    (void)min_e_unused;
    if (max_e < 1e-6f) {
        err = "degenerate bounds";
        return false;
    }
    const float scale =
        (target_scale_meters > 1e-4f) ? (target_scale_meters / max_e) : 1.0f;
    uniform_scale_positions(out_verts, scale);

    PTV_INFO("GLB loaded: {} verts, {} tris, scale {:.3f} (target {:.2f} m)",
             out_verts.size(), out_indices.size() / 3, scale,
             target_scale_meters);
    return true;
}

}  // namespace ptv::gltf_mesh_load
