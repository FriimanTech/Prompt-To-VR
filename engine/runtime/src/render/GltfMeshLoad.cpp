// GLB loader for the VR runtime (static geometry).
//
// We merge all triangle primitives into one indexed triangle list, rescale
// to ``target_scale_meters`` on the longest axis of the model AABB, and fill
// position, normal, base-colour UV and colour per vertex.
//
// Materials are kept: every primitive becomes a ``SubMesh`` (an index range
// plus its material), the material's base-colour texture is reported as an
// ``ImageInfo`` (external uri, ASTC KTX2 twin, or embedded bytes) and the
// UVs are emitted with KHR_texture_transform already applied. The material's
// ``baseColorFactor`` is multiplied into the per-vertex colour (glTF:
// ``COLOR_0 * baseColorFactor * texture``), so the renderer only has to
// sample the texture and multiply by the vertex colour.
//
// Node transforms ARE applied: primitives are baked into the world space of
// the node that instances them, and a mesh instanced by several nodes is
// emitted once per node. Two deliberate exceptions:
//
//   * A node carrying a ``skin`` contributes its geometry with an identity
//     transform, because the glTF spec says a skinned mesh node's transform
//     must be ignored -- the joint matrices place the mesh. This keeps a
//     rigged asset loaded through this static path identical to its bind
//     pose in GltfSkinnedLoad.cpp.
//   * A file whose scene graph reaches no meshes at all falls back to
//     emitting every mesh untransformed, which is what this loader did
//     before node support existed. Tripo output and ptv_glb_optimize output
//     are both single identity nodes, so neither path changes for them.
//
// IMPORTANT: When ``TINYGLTF_NO_STB_IMAGE`` is set, tinygltf REQUIRES a
// LoadImageData callback registered via ``TinyGLTF::SetImageLoader``,
// otherwise ``LoadBinaryFromFile`` aborts with
// "No LoadImageData callback specified.". We register a callback that does
// not decode pixels (it keeps the encoded bytes when a caller wants them),
// so the rest of the GLB parses cleanly. Pixel decoding is the
// TextureCache's job, once per unique image.
//   Reference: tinygltf README, "Optional Compile flags" section.
//
// This is the one translation unit that instantiates tinygltf.

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
// External image uris are kept as uris and resolved by the engine through
// the asset source: inside an APK the default file reader cannot see them,
// and on desktop it would read every texture into memory for nothing.
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "GltfAccessors.h"
#include "GltfMaterials.h"
#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/render/GltfMeshLoad.h"

namespace ptv::gltf_mesh_load {
namespace {

using namespace ptv::gltf_accessors;

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

/** One mesh to emit, plus the world transform to bake into it. */
struct MeshInstance {
    int mesh = -1;
    glm::mat4 world{1.0f};
    bool identity = true;
};

std::vector<MeshInstance> collect_mesh_instances(const tinygltf::Model& model) {
    std::vector<MeshInstance> out;
    const std::vector<int> parents = build_node_parents(model);
    const std::vector<glm::mat4> world = build_node_world_matrices(model, parents);

    for (size_t i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        if (n.mesh < 0 || static_cast<size_t>(n.mesh) >= model.meshes.size())
            continue;
        MeshInstance mi;
        mi.mesh = n.mesh;
        // Skinned nodes: joints own the placement, so the node transform is
        // deliberately dropped (glTF 2.0, "Skins").
        mi.world = (n.skin >= 0) ? glm::mat4(1.0f) : world[i];
        mi.identity = (mi.world == glm::mat4(1.0f));
        out.push_back(mi);
    }

    if (out.empty()) {
        // No node references a mesh (malformed or node-less export). Emit
        // everything untransformed rather than dropping the asset.
        for (size_t m = 0; m < model.meshes.size(); ++m) {
            MeshInstance mi;
            mi.mesh = static_cast<int>(m);
            out.push_back(mi);
        }
    }
    return out;
}

bool parse_glb_model(const std::string& path, tinygltf::Model& model,
                     bool want_image_bytes, std::string& err) {
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(want_image_bytes ? capture_encoded_image_loader
                                           : noop_image_loader,
                          nullptr);
    std::string warn;
    // Read the GLB bytes through the process-global asset source (filesystem
    // on desktop, APK/AAssetManager on Quest) and parse from memory, so the
    // loader is not tied to a filesystem path that does not exist on-device.
    // Mapped, not copied, on Quest (see IAssetSource::map); released as soon
    // as tinygltf has parsed it.
    auto bytes = asset_source().map(path);
    if (!bytes) {
        err = "asset source could not read GLB: " + path;
        return false;
    }
    const std::string base_dir = std::filesystem::path(path).parent_path().string();
    if (!loader.LoadBinaryFromMemory(&model, &err, &warn, bytes->data,
                                     static_cast<unsigned int>(bytes->size),
                                     base_dir)) {
        if (warn.size()) PTV_WARN("tinygltf warn: {}", warn);
        return false;
    }
    if (warn.size()) PTV_WARN("tinygltf warn: {}", warn);
    return true;
}

}  // namespace

std::string content_hash_hex(const unsigned char* data, std::size_t size) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= static_cast<std::uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string resolve_relative_uri(const std::string& glb_path,
                                 const std::string& uri) {
    if (uri.empty()) return uri;
    // Absolute uris (a drive letter or a leading slash) are used as is.
    if (uri.front() == '/' || (uri.size() > 1 && uri[1] == ':')) return uri;
    std::string base = glb_path;
    for (auto& c : base) {
        if (c == '\\') c = '/';
    }
    const auto slash = base.find_last_of('/');
    base = (slash == std::string::npos) ? std::string() : base.substr(0, slash);
    std::string joined = base.empty() ? uri : base + "/" + uri;
    for (auto& c : joined) {
        if (c == '\\') c = '/';
    }
    // Normalise "." and ".." without touching the filesystem: APK asset
    // paths have no directories to stat.
    const bool absolute = !joined.empty() && joined.front() == '/';
    std::vector<std::string> parts;
    std::string seg;
    auto flush = [&]() {
        if (seg.empty() || seg == ".") {
        } else if (seg == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(seg);
            }
        } else {
            parts.push_back(seg);
        }
        seg.clear();
    };
    for (char c : joined) {
        if (c == '/') {
            flush();
        } else {
            seg.push_back(c);
        }
    }
    flush();
    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "/";
        out += parts[i];
    }
    return out;
}

bool load_glb_mesh(const std::string& path,
                   float target_scale_meters,
                   std::vector<LitVertex>& out_verts,
                   std::vector<uint32_t>& out_indices,
                   MeshMaterials* out_materials,
                   std::string& err,
                   float unit_scale,
                   const LoadOptions& options) {
    out_verts.clear();
    out_indices.clear();
    tinygltf::Model model;
    if (!parse_glb_model(path, model, options.want_image_bytes, err)) return false;

    MeshMaterials local_mats;
    MeshMaterials& mats = out_materials ? *out_materials : local_mats;
    mats = MeshMaterials{};
    std::vector<gltf_materials::UvTransform> uv_xforms;
    gltf_materials::parse_materials(model, options.want_image_bytes, mats, uv_xforms);

    for (const auto& inst : collect_mesh_instances(model)) {
        const auto& mesh = model.meshes[static_cast<size_t>(inst.mesh)];
        // Normals need the inverse transpose so non-uniformly scaled nodes
        // still light correctly.
        const glm::mat3 normal_mat =
            inst.identity ? glm::mat3(1.0f)
                          : glm::transpose(glm::inverse(glm::mat3(inst.world)));

        for (const auto& prim : mesh.primitives) {
            // mode == -1 means triangles in glTF 2.0 default.
            if (!(prim.mode == TINYGLTF_MODE_TRIANGLES || prim.mode == -1)) {
                continue;
            }
            auto pos_it = prim.attributes.find("POSITION");
            if (pos_it == prim.attributes.end()) continue;

            std::string acc_err;
            std::vector<float> positions;
            size_t pos_comps = 0;
            if (!read_accessor_floats(model, pos_it->second, positions,
                                      &pos_comps, &acc_err) ||
                pos_comps != 3) {
                PTV_WARN("{}: skipping primitive ({})", path, acc_err);
                continue;
            }
            const size_t vcount = positions.size() / 3;

            std::vector<float> normals;
            auto n_it = prim.attributes.find("NORMAL");
            if (n_it != prim.attributes.end()) {
                size_t nc = 0;
                if (!read_accessor_floats(model, n_it->second, normals, &nc,
                                          &acc_err) ||
                    nc != 3) {
                    normals.clear();
                }
            }

            // Optional per-vertex COLOR_0 stream. When present, glTF spec
            // says we must multiply by baseColorFactor.
            std::vector<float> vert_colors;
            size_t color_comps = 0;
            auto c_it = prim.attributes.find("COLOR_0");
            if (c_it != prim.attributes.end()) {
                if (!read_accessor_floats(model, c_it->second, vert_colors,
                                          &color_comps, &acc_err)) {
                    vert_colors.clear();
                    color_comps = 0;
                } else {
                    mats.had_vertex_colors = true;
                }
            }

            const int mat_slot = gltf_materials::material_slot(mats, prim.material);
            const MaterialInfo& mat = mats.materials[static_cast<size_t>(mat_slot)];
            const auto& uv_xform = uv_xforms[static_cast<size_t>(mat_slot)];

            // The base-colour texture's UV set (TEXCOORD_0 for untextured
            // materials, so a later texture still lines up).
            std::vector<float> uvs;
            auto uv_it = prim.attributes.find(
                "TEXCOORD_" + std::to_string(std::max(0, mat.base_color_texcoord)));
            if (uv_it != prim.attributes.end()) {
                size_t uc = 0;
                if (!read_accessor_floats(model, uv_it->second, uvs, &uc, &acc_err) ||
                    uc != 2) {
                    uvs.clear();
                }
            }
            if (mat.base_color_image >= 0 && uvs.size() < vcount * 2) {
                mats.missing_uvs = true;
            }

            const float* factor = mat.base_color_factor;
            const uint32_t base = static_cast<uint32_t>(out_verts.size());
            for (size_t i = 0; i < vcount; ++i) {
                LitVertex lv{};
                glm::vec3 p(positions[i * 3 + 0], positions[i * 3 + 1],
                            positions[i * 3 + 2]);
                if (!inst.identity) {
                    p = glm::vec3(inst.world * glm::vec4(p, 1.0f));
                }
                lv.pos[0] = p.x;
                lv.pos[1] = p.y;
                lv.pos[2] = p.z;

                glm::vec3 nrm(0.0f, 1.0f, 0.0f);
                if (i * 3 + 2 < normals.size()) {
                    nrm = glm::vec3(normals[i * 3 + 0], normals[i * 3 + 1],
                                    normals[i * 3 + 2]);
                    if (!inst.identity) nrm = normal_mat * nrm;
                }
                lv.normal[0] = nrm.x;
                lv.normal[1] = nrm.y;
                lv.normal[2] = nrm.z;

                if (i * 2 + 1 < uvs.size()) {
                    float u = uvs[i * 2 + 0];
                    float v = uvs[i * 2 + 1];
                    uv_xform.apply(u, v);
                    lv.uv[0] = u;
                    lv.uv[1] = v;
                }

                for (int k = 0; k < 4; ++k) {
                    float cv = 1.0f;
                    if (color_comps >= 3 &&
                        static_cast<size_t>(k) < color_comps &&
                        i * color_comps + static_cast<size_t>(k) <
                            vert_colors.size()) {
                        cv = vert_colors[i * color_comps +
                                         static_cast<size_t>(k)];
                    }
                    lv.color[k] = options.bake_base_color_factor ? cv * factor[k] : cv;
                }
                out_verts.push_back(lv);
            }

            const size_t first_index = out_indices.size();
            if (prim.indices >= 0) {
                std::vector<uint32_t> idx;
                size_t ic = 0;
                if (!read_accessor_uints(model, prim.indices, idx, &ic,
                                         &acc_err) ||
                    ic != 1) {
                    // Drop vertices we added for this primitive if indices bad
                    out_verts.resize(base);
                    continue;
                }
                for (uint32_t id : idx) {
                    out_indices.push_back(base + id);
                }
            } else {
                // Non-indexed triangles
                if (vcount % 3 != 0) {
                    out_verts.resize(base);
                    continue;
                }
                for (size_t i = 0; i < vcount; i += 3) {
                    out_indices.push_back(base + static_cast<uint32_t>(i));
                    out_indices.push_back(base + static_cast<uint32_t>(i + 1));
                    out_indices.push_back(base + static_cast<uint32_t>(i + 2));
                }
            }
            const uint32_t added = static_cast<uint32_t>(out_indices.size() - first_index);
            if (added > 0) {
                // Adjacent primitives sharing a material merge into one range.
                if (!mats.submeshes.empty() && mats.submeshes.back().material == mat_slot &&
                    mats.submeshes.back().first_index + mats.submeshes.back().index_count ==
                        first_index) {
                    mats.submeshes.back().index_count += added;
                } else {
                    mats.submeshes.push_back(
                        SubMesh{static_cast<uint32_t>(first_index), added, mat_slot});
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
    // A precomputed unit scale wins: it was measured once, from LOD0, so
    // every LOD of the asset gets the same factor. Re-measuring each file
    // lets a coarse level whose bounds shifted come out a different size.
    const float scale =
        (unit_scale > 1e-8f)
            ? unit_scale
            : ((target_scale_meters > 1e-4f) ? (target_scale_meters / max_e)
                                             : 1.0f);
    uniform_scale_positions(out_verts, scale);

    int textured = 0;
    for (const auto& sm : mats.submeshes) {
        if (mats.materials[static_cast<size_t>(sm.material)].base_color_image >= 0) ++textured;
    }
    PTV_INFO("GLB loaded: {} verts, {} tris, scale {:.3f} (target {:.2f} m), "
             "{} submesh(es), {} textured, {} image(s)",
             out_verts.size(), out_indices.size() / 3, scale,
             target_scale_meters, mats.submeshes.size(), textured,
             mats.images.size());
    return true;
}

bool load_glb_merged(const std::string& path,
                     float target_scale_meters,
                     std::vector<LitVertex>& out_verts,
                     std::vector<uint32_t>& out_indices,
                     std::string& err,
                     float unit_scale) {
    return load_glb_mesh(path, target_scale_meters, out_verts, out_indices,
                         nullptr, err, unit_scale);
}

bool load_glb_textured(const std::string& path, TexturedMesh& out,
                       std::string& err) {
    out = TexturedMesh{};
    std::vector<LitVertex> verts;
    std::vector<uint32_t> indices;
    MeshMaterials mats;
    LoadOptions opts;
    opts.want_image_bytes = true;
    // Source scale (unit_scale 1): socket tools measure in GLB units.
    if (!load_glb_mesh(path, 0.0f, verts, indices, &mats, err, 1.0f, opts)) {
        return false;
    }
    out.verts.resize(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        auto& tv = out.verts[i];
        std::memcpy(tv.pos, verts[i].pos, sizeof(tv.pos));
        std::memcpy(tv.normal, verts[i].normal, sizeof(tv.normal));
        std::memcpy(tv.uv, verts[i].uv, sizeof(tv.uv));
        std::memcpy(tv.color, verts[i].color, sizeof(tv.color));
    }
    out.indices = std::move(indices);
    out.tri_image.assign(out.indices.size() / 3, -1);

    // External images (a decimated LOD) are read through the asset source,
    // preferring the original PNG/JPEG over the KTX2 twin (tools decode with
    // stb, which does not read KTX2).
    std::vector<int> slot(mats.images.size(), -1);
    for (size_t i = 0; i < mats.images.size(); ++i) {
        auto& img = mats.images[i];
        EncodedImage e;
        e.mime = img.mime;
        if (!img.bytes.empty()) {
            e.bytes = img.bytes;
        } else if (!img.uri.empty()) {
            auto file = asset_source().read_bytes(resolve_relative_uri(path, img.uri));
            if (file) e.bytes.assign(file->begin(), file->end());
        }
        if (e.bytes.empty()) continue;
        out.images.push_back(std::move(e));
        slot[i] = static_cast<int>(out.images.size()) - 1;
    }
    for (const auto& sm : mats.submeshes) {
        const int img = mats.materials[static_cast<size_t>(sm.material)].base_color_image;
        const int s = img >= 0 ? slot[static_cast<size_t>(img)] : -1;
        for (uint32_t t = sm.first_index / 3; t < (sm.first_index + sm.index_count) / 3; ++t) {
            out.tri_image[t] = s;
        }
    }
    return true;
}

}  // namespace ptv::gltf_mesh_load
