// ptv_glb_optimize: offline GLB decimation + LOD generation.
//
// Used by the orchestrator's ``asset_optimization`` stage to fit generated
// (Tripo / Uthana) assets into the per-project triangle budget before they
// ever reach the headset. Raw Tripo output is routinely ~250k verts / ~500k
// triangles per asset, which blows straight past Meta's Quest 3 guidance of
// 1.3M-1.8M triangles for the WHOLE frame.
//
// Colour is preserved, not baked away:
//   * Geometry loads through the runtime's own loaders
//     (load_glb_mesh / load_glb_skinned) WITH materials: every primitive's
//     material, base-colour texture, UVs (KHR_texture_transform applied),
//     baseColorFactor, alpha mode and raw COLOR_0 come along.
//   * Vertices are welded per material (a material id rides as a second
//     stream), so a vertex is never shared by two materials and material
//     boundaries survive simplification as seams.
//   * meshopt_simplifyWithAttributes weighs normals, UVs (weight =
//     1/sqrt(average UV triangle area), meshoptimizer's recommendation) and
//     colours. In the default (non-permissive) mode the simplifier only
//     collapses ALONG UV seams, so texture islands keep their borders.
//   * Triangles are regrouped by material and written as one primitive per
//     material; the output references each base-colour image by uri
//     (``--textures`` map from the stage: shared, hash-named files plus an
//     ASTC KTX2 twin via the ``PTV_texture_astc`` texture extension), or
//     embeds it when no map is given. The normal / metallic-roughness maps
//     are not referenced: the renderer is base-colour only, and the source
//     GLB keeps them on disk.
//
// RIGGED input takes the same path with the skeleton carried through:
//   * Geometry loads as SkinnedVertex, so JOINTS_0 / WEIGHTS_0 ride along
//     inside the interleaved struct the remap reorders -- meshopt never
//     invents a vertex, so bindings stay attached. Bone data is kept OUT of
//     the attribute metric (categorical / partition of unity), and
//     meshopt_SimplifyRegularize keeps triangles well shaped under
//     deformation.
//   * --sloppy is REFUSED for rigged meshes: sloppy simplification invents
//     vertex positions that carry no bindings.
//   * The skeleton (joint nodes, hierarchy, bind TRS, inverse binds) and the
//     animation clips are copied verbatim into EVERY LOD, so one palette
//     serves all levels and a mid-animation LOD switch is safe.
//
// Extra modes for the stage:
//   --list-images            print the base-colour images (hash, mime, size,
//                            materials) as JSON and exit
//   --extract-images <dir>   also write each one as <dir>/<hash>.<ext>
//
// The GLB container is written by hand (nlohmann::json + the binary chunk)
// rather than through tinygltf's writer: the runtime already compiles
// TINYGLTF_IMPLEMENTATION, so pulling the writer in here would risk ODR
// clashes, and we want byte-exact control over the layout we emit.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "ptv/render/GltfMeshLoad.h"
#include "ptv/render/GltfSkinnedLoad.h"

namespace {

namespace gml = ptv::gltf_mesh_load;
using gml::LitVertex;
using gml::MeshMaterials;
using gml::SkeletonData;
using gml::SkinnedVertex;
using gml::SubMesh;

static_assert(offsetof(LitVertex, pos) == offsetof(SkinnedVertex, pos) &&
                  offsetof(LitVertex, normal) == offsetof(SkinnedVertex, normal) &&
                  offsetof(LitVertex, uv) == offsetof(SkinnedVertex, uv) &&
                  offsetof(LitVertex, color) == offsetof(SkinnedVertex, color),
              "LitVertex and SkinnedVertex share their leading fields");

struct Options {
    std::string input;
    std::string output;
    std::string report;
    std::size_t target_tris = 0;
    bool allow_sloppy = false;
    // JSON file {"triangles": [...]} of merged-mesh triangles to remove
    // before simplification (a modelled bowstring the engine draws itself).
    std::string drop_triangles;
    std::string textures_map;
    bool list_images = false;
    std::string extract_dir;
};

void print_usage() {
    std::fprintf(
        stderr,
        "usage: ptv_glb_optimize --input <in.glb> --output <out.glb>\n"
        "                       --target-tris <N> [--sloppy]\n"
        "                       [--textures <map.json>] [--report <r.json>]\n"
        "                       [--drop-triangles <ids.json>]\n"
        "       ptv_glb_optimize --input <in.glb> --list-images\n"
        "                       [--extract-images <dir>]\n");
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        if (a == "--input") {
            if (!next(opt.input)) return false;
        } else if (a == "--output") {
            if (!next(opt.output)) return false;
        } else if (a == "--report") {
            if (!next(opt.report)) return false;
        } else if (a == "--target-tris") {
            std::string v;
            if (!next(v)) return false;
            opt.target_tris = static_cast<std::size_t>(std::stoull(v));
        } else if (a == "--sloppy") {
            opt.allow_sloppy = true;
        } else if (a == "--drop-triangles") {
            if (!next(opt.drop_triangles)) return false;
        } else if (a == "--textures") {
            if (!next(opt.textures_map)) return false;
        } else if (a == "--list-images") {
            opt.list_images = true;
        } else if (a == "--extract-images") {
            if (!next(opt.extract_dir)) return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    if (opt.input.empty()) return false;
    if (opt.list_images) return true;
    return !opt.output.empty() && opt.target_tris > 0;
}

std::string ext_for_mime(const std::string& mime) {
    if (mime == "image/png") return "png";
    if (mime == "image/jpeg") return "jpg";
    if (mime == "image/ktx2") return "ktx2";
    if (mime == "image/webp") return "webp";
    return "bin";
}

std::string mime_for_bytes(const std::vector<unsigned char>& b, const std::string& declared) {
    if (!declared.empty()) return declared;
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        return "image/png";
    }
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "image/jpeg";
    return "application/octet-stream";
}

/** Bytes of every base-colour image (embedded, or an external file). */
void fill_image_bytes(const std::string& input, MeshMaterials& mats) {
    for (auto& img : mats.images) {
        if (img.bytes.empty() && !img.uri.empty()) {
            std::ifstream f(gml::resolve_relative_uri(input, img.uri), std::ios::binary);
            if (f) {
                img.bytes.assign(std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>());
            }
        }
        if (!img.bytes.empty()) {
            img.mime = mime_for_bytes(img.bytes, img.mime);
            if (img.hash.empty()) {
                img.hash = gml::content_hash_hex(img.bytes.data(), img.bytes.size());
            }
        }
    }
}

// --- Texture map -----------------------------------------------------------

struct TextureEntry {
    std::string uri;
    std::string ktx2;
    std::string mime;
};

bool load_texture_map(const std::string& path,
                      std::unordered_map<std::string, TextureEntry>& out) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot read texture map %s\n", path.c_str());
        return false;
    }
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object() || !j.contains("images") || !j["images"].is_object()) {
        std::fprintf(stderr, "texture map %s has no \"images\" object\n", path.c_str());
        return false;
    }
    for (auto it = j["images"].begin(); it != j["images"].end(); ++it) {
        TextureEntry e;
        e.uri = it.value().value("uri", "");
        e.ktx2 = it.value().value("ktx2", "");
        e.mime = it.value().value("mime", "");
        out[it.key()] = e;
    }
    return true;
}

// --- Optimisation ----------------------------------------------------------

template <class V>
struct Optimized {
    std::vector<V> verts;
    std::vector<std::uint32_t> indices;
    std::vector<SubMesh> submeshes;
    float simplify_error{0.0f};
    float uv_weight{0.0f};
    bool used_sloppy{false};
};

std::vector<int> triangle_materials(const MeshMaterials& mats, std::size_t tri_count) {
    std::vector<int> tm(tri_count, mats.materials.empty()
                                       ? 0
                                       : static_cast<int>(mats.materials.size()) - 1);
    for (const auto& sm : mats.submeshes) {
        for (std::uint32_t t = sm.first_index / 3;
             t < (sm.first_index + sm.index_count) / 3 && t < tri_count; ++t) {
            tm[t] = sm.material;
        }
    }
    return tm;
}

template <class V>
bool optimize_mesh(std::vector<V> verts, std::vector<std::uint32_t> indices,
                   std::vector<int> tri_mat, const MeshMaterials& mats,
                   std::size_t target_tris, bool skinned, bool allow_sloppy,
                   Optimized<V>& out, std::string& err) {
    const std::size_t tri_count = indices.size() / 3;
    // Material per vertex. The loaders emit each primitive's vertices once,
    // so a vertex has one material; a file that shares one between two
    // materials gets a duplicate so the invariant holds.
    std::vector<std::uint32_t> vmat(verts.size(), UINT32_MAX);
    for (std::size_t t = 0; t < tri_count; ++t) {
        for (int k = 0; k < 3; ++k) {
            std::uint32_t& vi = indices[t * 3 + static_cast<std::size_t>(k)];
            const auto m = static_cast<std::uint32_t>(tri_mat[t]);
            if (vmat[vi] == UINT32_MAX) {
                vmat[vi] = m;
            } else if (vmat[vi] != m) {
                verts.push_back(verts[vi]);
                vmat.push_back(m);
                vi = static_cast<std::uint32_t>(verts.size() - 1);
            }
        }
    }
    for (auto& m : vmat) {
        if (m == UINT32_MAX) m = 0;
    }

    // Weld duplicates of the same vertex in the same material.
    const meshopt_Stream streams[2] = {
        {verts.data(), sizeof(V), sizeof(V)},
        {vmat.data(), sizeof(std::uint32_t), sizeof(std::uint32_t)},
    };
    std::vector<unsigned int> remap(verts.size());
    const std::size_t unique = meshopt_generateVertexRemapMulti(
        remap.data(), indices.data(), indices.size(), verts.size(), streams, 2);
    std::vector<V> welded(unique);
    std::vector<std::uint32_t> welded_mat(unique);
    std::vector<unsigned int> welded_idx(indices.size());
    meshopt_remapVertexBuffer(welded.data(), verts.data(), verts.size(), sizeof(V),
                              remap.data());
    meshopt_remapVertexBuffer(welded_mat.data(), vmat.data(), vmat.size(),
                              sizeof(std::uint32_t), remap.data());
    meshopt_remapIndexBuffer(welded_idx.data(), indices.data(), indices.size(),
                             remap.data());

    // UV weight from texel density (only triangles that sample a texture).
    double uv_area = 0.0;
    std::size_t uv_tris = 0;
    for (std::size_t t = 0; t < tri_count; ++t) {
        const int m = static_cast<int>(welded_mat[welded_idx[t * 3]]);
        if (m < 0 || static_cast<std::size_t>(m) >= mats.materials.size() ||
            mats.materials[static_cast<std::size_t>(m)].base_color_image < 0) {
            continue;
        }
        const V& a = welded[welded_idx[t * 3 + 0]];
        const V& b = welded[welded_idx[t * 3 + 1]];
        const V& c = welded[welded_idx[t * 3 + 2]];
        const double ux = b.uv[0] - a.uv[0], uy = b.uv[1] - a.uv[1];
        const double vx = c.uv[0] - a.uv[0], vy = c.uv[1] - a.uv[1];
        uv_area += std::abs(ux * vy - uy * vx) * 0.5;
        ++uv_tris;
    }
    float uv_weight = 0.0f;
    if (uv_tris > 0 && uv_area > 0.0) {
        const double avg = uv_area / static_cast<double>(uv_tris);
        uv_weight = static_cast<float>(std::clamp(1.0 / std::sqrt(avg), 1.0, 200.0));
    }
    out.uv_weight = uv_weight;

    // Attribute stream: normal(3) uv(2) colour(4). Bone data deliberately
    // absent (see the header).
    constexpr std::size_t kAttr = 9;
    std::vector<float> attrs(welded.size() * kAttr);
    for (std::size_t i = 0; i < welded.size(); ++i) {
        float* d = &attrs[i * kAttr];
        std::memcpy(d, welded[i].normal, sizeof(float) * 3);
        std::memcpy(d + 3, welded[i].uv, sizeof(float) * 2);
        std::memcpy(d + 5, welded[i].color, sizeof(float) * 4);
    }
    const float weights[kAttr] = {0.5f, 0.5f, 0.5f, uv_weight, uv_weight,
                                  0.5f, 0.5f, 0.5f, 0.0f};

    const std::size_t target_index_count = target_tris * 3;
    std::vector<unsigned int> simplified(welded_idx.size());
    std::size_t result_count = welded_idx.size();
    if (target_index_count < welded_idx.size()) {
        float result_error = 0.0f;
        const unsigned int flags = skinned ? meshopt_SimplifyRegularize : 0u;
        result_count = meshopt_simplifyWithAttributes(
            simplified.data(), welded_idx.data(), welded_idx.size(),
            &welded[0].pos[0], welded.size(), sizeof(V), attrs.data(),
            kAttr * sizeof(float), weights, kAttr, /*vertex_lock=*/nullptr,
            target_index_count, /*target_error=*/1.0f, flags, &result_error);
        out.simplify_error = result_error;
        // The error-bounded simplifier preserves topology and can stall well
        // above aggressive targets. The sloppy fallback is for distant LODs
        // only (the stage passes --sloppy from LOD2 down) and never rigged.
        if (allow_sloppy && !skinned && result_count > target_index_count * 3 / 2) {
            std::vector<unsigned int> sloppy(welded_idx.size());
            float sloppy_error = 0.0f;
            const std::size_t sloppy_count = meshopt_simplifySloppy(
                sloppy.data(), welded_idx.data(), welded_idx.size(),
                &welded[0].pos[0], welded.size(), sizeof(V), target_index_count,
                /*target_error=*/1.0f, &sloppy_error);
            if (sloppy_count >= 3 && sloppy_count < result_count) {
                simplified = std::move(sloppy);
                result_count = sloppy_count;
                out.simplify_error = sloppy_error;
                out.used_sloppy = true;
            }
        }
        simplified.resize(result_count);
    } else {
        simplified = welded_idx;
        result_count = welded_idx.size();
    }
    if (result_count < 3) {
        err = "simplification produced no triangles";
        return false;
    }

    // Material of each surviving triangle. Collapses only merge vertices
    // joined by an edge, and edges never cross materials, so all three
    // corners must agree -- anything else is a bug worth failing on.
    const std::size_t out_tris = result_count / 3;
    std::vector<std::uint32_t> out_mat(out_tris);
    for (std::size_t t = 0; t < out_tris; ++t) {
        const std::uint32_t m0 = welded_mat[simplified[t * 3 + 0]];
        const std::uint32_t m1 = welded_mat[simplified[t * 3 + 1]];
        const std::uint32_t m2 = welded_mat[simplified[t * 3 + 2]];
        if (m0 != m1 || m0 != m2) {
            err = "simplification produced a triangle spanning two materials";
            return false;
        }
        out_mat[t] = m0;
    }

    // Regroup triangles by material (stable), one index range each.
    std::vector<std::size_t> order(out_tris);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return out_mat[a] < out_mat[b];
    });
    std::vector<unsigned int> grouped(result_count);
    for (std::size_t i = 0; i < out_tris; ++i) {
        std::memcpy(&grouped[i * 3], &simplified[order[i] * 3], sizeof(unsigned int) * 3);
    }
    std::vector<SubMesh> subs;
    for (std::size_t i = 0; i < out_tris; ++i) {
        const int m = static_cast<int>(out_mat[order[i]]);
        if (subs.empty() || subs.back().material != m) {
            subs.push_back(SubMesh{static_cast<std::uint32_t>(i * 3), 0, m});
        }
        subs.back().index_count += 3;
    }
    // GPU-friendly ordering inside each range.
    for (const auto& sm : subs) {
        unsigned int* r = &grouped[sm.first_index];
        meshopt_optimizeVertexCache(r, r, sm.index_count, welded.size());
        meshopt_optimizeOverdraw(r, r, sm.index_count, &welded[0].pos[0], welded.size(),
                                 sizeof(V), 1.05f);
    }
    // Compact: vertices end up ordered by first use, i.e. grouped by
    // material, which lets each primitive own a contiguous vertex range.
    std::vector<V> final_verts(welded.size());
    const std::size_t fv = meshopt_optimizeVertexFetch(
        final_verts.data(), grouped.data(), grouped.size(), welded.data(),
        welded.size(), sizeof(V));
    final_verts.resize(fv);

    out.verts = std::move(final_verts);
    out.indices.assign(grouped.begin(), grouped.end());
    out.submeshes = std::move(subs);
    return true;
}

// --- GLB writing -----------------------------------------------------------

void append_padded(std::vector<std::uint8_t>& bin, const void* data,
                   std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bin.insert(bin.end(), p, p + bytes);
    while (bin.size() % 4 != 0) bin.push_back(0);
}

/** Wrap a finished glTF JSON document + BIN blob in the GLB container. */
bool write_glb_container(const std::string& path, const nlohmann::json& doc,
                         const std::vector<std::uint8_t>& bin) {
    std::string json_text = doc.dump();
    while (json_text.size() % 4 != 0) json_text.push_back(' ');

    const std::uint32_t json_len = static_cast<std::uint32_t>(json_text.size());
    const std::uint32_t bin_len = static_cast<std::uint32_t>(bin.size());
    const std::uint32_t total = 12 + 8 + json_len + 8 + bin_len;

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot open output: %s\n", path.c_str());
        return false;
    }
    auto put_u32 = [&](std::uint32_t v) {
        out.write(reinterpret_cast<const char*>(&v), 4);
    };
    put_u32(0x46546C67u);  // 'glTF'
    put_u32(2);
    put_u32(total);
    put_u32(json_len);
    put_u32(0x4E4F534Au);  // 'JSON'
    out.write(json_text.data(), static_cast<std::streamsize>(json_len));
    put_u32(bin_len);
    put_u32(0x004E4942u);  // 'BIN\0'
    out.write(reinterpret_cast<const char*>(bin.data()),
              static_cast<std::streamsize>(bin_len));
    return out.good();
}

struct WriteStats {
    float min_pos[3]{0, 0, 0};
    float max_pos[3]{0, 0, 0};
    std::size_t textured_triangles{0};
    nlohmann::json materials = nlohmann::json::array();
    nlohmann::json images = nlohmann::json::array();
};

/** Materials, textures, samplers and images for the output document.
 *  Returns old material slot -> output material index. */
std::map<int, int> write_materials(nlohmann::json& doc, std::vector<std::uint8_t>& bin,
                                   const MeshMaterials& mats,
                                   const std::vector<SubMesh>& subs,
                                   const std::unordered_map<std::string, TextureEntry>* tmap,
                                   WriteStats& stats, bool& ok) {
    ok = true;
    std::map<int, int> mat_out;
    std::map<int, int> image_out;     // MeshMaterials image -> output image
    std::map<int, int> texture_out;   // MeshMaterials image -> output texture
    nlohmann::json materials = nlohmann::json::array();
    nlohmann::json textures = nlohmann::json::array();
    nlohmann::json images = nlohmann::json::array();
    nlohmann::json samplers = nlohmann::json::array();
    bool used_astc = false;

    for (const auto& sm : subs) {
        if (mat_out.count(sm.material)) continue;
        const auto& m = mats.materials[static_cast<std::size_t>(sm.material)];
        nlohmann::json mj;
        mj["name"] = m.name.empty() ? "material_" + std::to_string(sm.material) : m.name;
        nlohmann::json pbr;
        pbr["baseColorFactor"] = {m.base_color_factor[0], m.base_color_factor[1],
                                  m.base_color_factor[2], m.base_color_factor[3]};
        pbr["metallicFactor"] = 0.0;
        pbr["roughnessFactor"] = 1.0;
        nlohmann::json report{{"name", mj["name"]},
                              {"factor", pbr["baseColorFactor"]},
                              {"textured", m.base_color_image >= 0},
                              {"alpha_mode", static_cast<int>(m.alpha_mode)},
                              {"source_maps",
                               {{"normal", m.has_normal_map},
                                {"metallic_roughness", m.has_metallic_roughness_map},
                                {"occlusion", m.has_occlusion_map},
                                {"emissive", m.has_emissive_map}}}};
        if (m.base_color_image >= 0) {
            const int img_idx = m.base_color_image;
            const auto& img = mats.images[static_cast<std::size_t>(img_idx)];
            if (!texture_out.count(img_idx)) {
                nlohmann::json ij;
                ij["mimeType"] = img.mime.empty() ? "image/png" : img.mime;
                if (!img.name.empty()) ij["name"] = img.name;
                nlohmann::json tj;
                std::string ktx2;
                if (tmap != nullptr) {
                    auto it = tmap->find(img.hash);
                    if (img.hash.empty() || it == tmap->end()) {
                        std::fprintf(stderr,
                                     "texture map has no entry for image %d (hash %s) "
                                     "of material '%s'\n",
                                     img_idx, img.hash.c_str(), m.name.c_str());
                        ok = false;
                        return mat_out;
                    }
                    ij["uri"] = it->second.uri;
                    if (!it->second.mime.empty()) ij["mimeType"] = it->second.mime;
                    ktx2 = it->second.ktx2;
                } else {
                    if (img.bytes.empty()) {
                        std::fprintf(stderr,
                                     "base-colour image %d of material '%s' has no bytes "
                                     "to embed\n", img_idx, m.name.c_str());
                        ok = false;
                        return mat_out;
                    }
                    const std::size_t off = bin.size();
                    append_padded(bin, img.bytes.data(), img.bytes.size());
                    doc["bufferViews"].push_back(
                        {{"buffer", 0},
                         {"byteOffset", static_cast<std::uint64_t>(off)},
                         {"byteLength", static_cast<std::uint64_t>(img.bytes.size())}});
                    ij["bufferView"] = doc["bufferViews"].size() - 1;
                }
                images.push_back(ij);
                image_out[img_idx] = static_cast<int>(images.size()) - 1;

                nlohmann::json sj;
                if (m.sampler.mag_filter > 0) sj["magFilter"] = m.sampler.mag_filter;
                if (m.sampler.min_filter > 0) sj["minFilter"] = m.sampler.min_filter;
                sj["wrapS"] = m.sampler.wrap_s;
                sj["wrapT"] = m.sampler.wrap_t;
                samplers.push_back(sj);

                tj["source"] = image_out[img_idx];
                tj["sampler"] = samplers.size() - 1;
                if (!ktx2.empty()) {
                    tj["extensions"]["PTV_texture_astc"] = {{"uri", ktx2}};
                    used_astc = true;
                }
                textures.push_back(tj);
                texture_out[img_idx] = static_cast<int>(textures.size()) - 1;
                stats.images.push_back({{"hash", img.hash},
                                        {"mime", ij["mimeType"]},
                                        {"uri", ij.value("uri", "")},
                                        {"ktx2", ktx2},
                                        {"embedded", tmap == nullptr}});
            }
            pbr["baseColorTexture"] = {{"index", texture_out[img_idx]}, {"texCoord", 0}};
            report["image_hash"] = img.hash;
        }
        mj["pbrMetallicRoughness"] = pbr;
        if (m.alpha_mode == gml::AlphaMode::Mask) {
            mj["alphaMode"] = "MASK";
            mj["alphaCutoff"] = m.alpha_cutoff;
        } else if (m.alpha_mode == gml::AlphaMode::Blend) {
            mj["alphaMode"] = "BLEND";
        }
        mj["doubleSided"] = m.double_sided;
        materials.push_back(mj);
        mat_out[sm.material] = static_cast<int>(materials.size()) - 1;
        stats.materials.push_back(report);
    }
    doc["materials"] = materials;
    if (!textures.empty()) {
        doc["textures"] = textures;
        doc["images"] = images;
        doc["samplers"] = samplers;
    }
    if (used_astc) {
        doc["extensionsUsed"] = nlohmann::json::array({"PTV_texture_astc"});
    }
    return mat_out;
}

/** Per-primitive vertex range: [first, last] vertex referenced. */
struct VertexRange {
    std::uint32_t first{0};
    std::uint32_t count{0};
};

template <class V>
std::vector<VertexRange> vertex_ranges(const Optimized<V>& m, bool& contiguous) {
    contiguous = true;
    std::vector<VertexRange> out;
    std::uint32_t prev_end = 0;
    for (const auto& sm : m.submeshes) {
        std::uint32_t lo = UINT32_MAX, hi = 0;
        for (std::uint32_t i = 0; i < sm.index_count; ++i) {
            const std::uint32_t v = m.indices[sm.first_index + i];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (lo < prev_end) contiguous = false;
        prev_end = hi + 1;
        out.push_back(VertexRange{lo, hi - lo + 1});
    }
    return out;
}

/** Write the optimised mesh (static or rigged) as a GLB. */
template <class V>
bool write_glb_out(const std::string& path, const Optimized<V>& m,
                   const MeshMaterials& mats, bool write_colors,
                   const std::unordered_map<std::string, TextureEntry>* tmap,
                   const SkeletonData* skel, WriteStats& stats) {
    bool contiguous = true;
    const auto ranges = vertex_ranges(m, contiguous);
    if (!contiguous) {
        std::fprintf(stderr, "internal: material vertex ranges overlap\n");
        return false;
    }
    const std::size_t n = m.verts.size();
    std::vector<float> positions(n * 3), normals(n * 3), uvs(n * 2), colors(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        std::memcpy(&positions[i * 3], m.verts[i].pos, sizeof(float) * 3);
        std::memcpy(&normals[i * 3], m.verts[i].normal, sizeof(float) * 3);
        std::memcpy(&uvs[i * 2], m.verts[i].uv, sizeof(float) * 2);
        std::memcpy(&colors[i * 4], m.verts[i].color, sizeof(float) * 4);
    }
    // Indices rebased to their primitive's vertex range.
    std::vector<std::uint32_t> local_idx(m.indices.size());
    for (std::size_t s = 0; s < m.submeshes.size(); ++s) {
        const auto& sm = m.submeshes[s];
        for (std::uint32_t i = 0; i < sm.index_count; ++i) {
            local_idx[sm.first_index + i] = m.indices[sm.first_index + i] - ranges[s].first;
        }
    }

    std::vector<std::uint8_t> joints;
    std::vector<float> weights;
    std::vector<float> ibm;
    if constexpr (std::is_same_v<V, SkinnedVertex>) {
        joints.resize(n * 4);
        weights.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            std::memcpy(&joints[i * 4], m.verts[i].joints, 4);
            // The quantizer made the four bytes sum to 255, so this
            // round-trips exactly.
            for (int k = 0; k < 4; ++k) {
                weights[i * 4 + static_cast<std::size_t>(k)] =
                    static_cast<float>(m.verts[i].weights[k]) / 255.0f;
            }
        }
        ibm.resize(skel->joint_count() * 16);
        for (std::size_t j = 0; j < skel->joint_count(); ++j) {
            std::memcpy(&ibm[j * 16], &skel->inverse_bind[j][0][0], sizeof(float) * 16);
        }
    }

    std::vector<std::uint8_t> bin;
    nlohmann::json doc;
    doc["asset"] = {{"version", "2.0"}, {"generator", "ptv_glb_optimize"}};
    doc["bufferViews"] = nlohmann::json::array();
    doc["accessors"] = nlohmann::json::array();
    auto add_view = [&](const void* data, std::size_t bytes, int target,
                        int stride = 0) -> int {
        const std::size_t off = bin.size();
        append_padded(bin, data, bytes);
        nlohmann::json v{{"buffer", 0},
                         {"byteOffset", static_cast<std::uint64_t>(off)},
                         {"byteLength", static_cast<std::uint64_t>(bytes)}};
        if (target != 0) v["target"] = target;
        if (stride != 0) v["byteStride"] = stride;
        doc["bufferViews"].push_back(v);
        return static_cast<int>(doc["bufferViews"].size()) - 1;
    };
    // 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER.
    const int v_idx = add_view(local_idx.data(), local_idx.size() * 4, 34963);
    const int v_pos = add_view(positions.data(), positions.size() * 4, 34962);
    const int v_nrm = add_view(normals.data(), normals.size() * 4, 34962);
    const int v_uv = add_view(uvs.data(), uvs.size() * 4, 34962);
    const int v_col = write_colors ? add_view(colors.data(), colors.size() * 4, 34962) : -1;
    int v_jnt = -1, v_wgt = -1, v_ibm = -1;
    if constexpr (std::is_same_v<V, SkinnedVertex>) {
        // byteStride is required on a vertex view whose elements are smaller
        // than 4 bytes apart... JOINTS_0 is 4 bytes per vertex already.
        v_jnt = add_view(joints.data(), joints.size(), 34962);
        v_wgt = add_view(weights.data(), weights.size() * 4, 34962);
        v_ibm = add_view(ibm.data(), ibm.size() * 4, 0);
    }

    WriteStats& st = stats;
    for (int k = 0; k < 3; ++k) {
        st.min_pos[k] = std::numeric_limits<float>::max();
        st.max_pos[k] = std::numeric_limits<float>::lowest();
    }

    bool mats_ok = true;
    const auto mat_out = write_materials(doc, bin, mats, m.submeshes, tmap, st, mats_ok);
    if (!mats_ok) return false;

    // 5121 = UNSIGNED_BYTE, 5125 = UNSIGNED_INT, 5126 = FLOAT.
    nlohmann::json prims = nlohmann::json::array();
    for (std::size_t s = 0; s < m.submeshes.size(); ++s) {
        const auto& sm = m.submeshes[s];
        const auto& r = ranges[s];
        float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
        float hi[3] = {std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest()};
        for (std::uint32_t v = r.first; v < r.first + r.count; ++v) {
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], m.verts[v].pos[k]);
                hi[k] = std::max(hi[k], m.verts[v].pos[k]);
            }
        }
        for (int k = 0; k < 3; ++k) {
            st.min_pos[k] = std::min(st.min_pos[k], lo[k]);
            st.max_pos[k] = std::max(st.max_pos[k], hi[k]);
        }
        auto accessor = [&](int view, std::size_t elem_bytes, int comp, const char* type,
                            std::size_t count, std::size_t first) {
            nlohmann::json a{{"bufferView", view},
                             {"byteOffset", static_cast<std::uint64_t>(first * elem_bytes)},
                             {"componentType", comp},
                             {"count", static_cast<std::uint64_t>(count)},
                             {"type", type}};
            doc["accessors"].push_back(a);
            return static_cast<int>(doc["accessors"].size()) - 1;
        };
        const int a_idx = accessor(v_idx, 4, 5125, "SCALAR", sm.index_count, sm.first_index);
        const int a_pos = accessor(v_pos, 12, 5126, "VEC3", r.count, r.first);
        doc["accessors"][static_cast<std::size_t>(a_pos)]["min"] = {lo[0], lo[1], lo[2]};
        doc["accessors"][static_cast<std::size_t>(a_pos)]["max"] = {hi[0], hi[1], hi[2]};
        const int a_nrm = accessor(v_nrm, 12, 5126, "VEC3", r.count, r.first);
        const int a_uv = accessor(v_uv, 8, 5126, "VEC2", r.count, r.first);
        nlohmann::json attrs{{"POSITION", a_pos}, {"NORMAL", a_nrm}, {"TEXCOORD_0", a_uv}};
        if (v_col >= 0) attrs["COLOR_0"] = accessor(v_col, 16, 5126, "VEC4", r.count, r.first);
        if constexpr (std::is_same_v<V, SkinnedVertex>) {
            attrs["JOINTS_0"] = accessor(v_jnt, 4, 5121, "VEC4", r.count, r.first);
            attrs["WEIGHTS_0"] = accessor(v_wgt, 16, 5126, "VEC4", r.count, r.first);
        }
        const int mat_index = mat_out.at(sm.material);
        prims.push_back({{"attributes", attrs},
                         {"indices", a_idx},
                         {"material", mat_index},
                         {"mode", 4}});
        if (mats.materials[static_cast<std::size_t>(sm.material)].base_color_image >= 0) {
            st.textured_triangles += sm.index_count / 3;
        }
    }
    doc["meshes"] = nlohmann::json::array({{{"primitives", prims}}});

    if constexpr (!std::is_same_v<V, SkinnedVertex>) {
        doc["nodes"] = nlohmann::json::array({{{"mesh", 0}}});
        doc["scenes"] = nlohmann::json::array({{{"nodes", {0}}}});
        doc["scene"] = 0;
    } else {
        const std::size_t joint_count = skel->joint_count();
        const int a_ibm = static_cast<int>(doc["accessors"].size());
        doc["accessors"].push_back({{"bufferView", v_ibm},
                                    {"componentType", 5126},
                                    {"count", static_cast<std::uint64_t>(joint_count)},
                                    {"type", "MAT4"}});

        // Node 0 = skinned mesh, nodes 1..N = joints in skin order, so every
        // JOINTS_0 index stays valid without remapping.
        nlohmann::json nodes = nlohmann::json::array();
        nodes.push_back({{"name", "ptv_skinned_mesh"}, {"mesh", 0}, {"skin", 0}});
        std::vector<std::vector<int>> children(joint_count);
        std::vector<int> roots;
        for (std::size_t j = 0; j < joint_count; ++j) {
            const int parent = skel->parents[j];
            if (parent >= 0) {
                children[static_cast<std::size_t>(parent)].push_back(static_cast<int>(j));
            } else {
                roots.push_back(static_cast<int>(j));
            }
        }
        nlohmann::json joint_indices = nlohmann::json::array();
        for (std::size_t j = 0; j < joint_count; ++j) {
            const auto& bp = skel->bind_pose[j];
            nlohmann::json node{
                {"name", skel->names[j]},
                {"translation", {bp.translation.x, bp.translation.y, bp.translation.z}},
                // glTF stores quaternions x, y, z, w.
                {"rotation", {bp.rotation.x, bp.rotation.y, bp.rotation.z, bp.rotation.w}},
                {"scale", {bp.scale.x, bp.scale.y, bp.scale.z}},
            };
            if (!children[j].empty()) {
                nlohmann::json kids = nlohmann::json::array();
                for (int c : children[j]) kids.push_back(c + 1);
                node["children"] = kids;
            }
            nodes.push_back(node);
            joint_indices.push_back(static_cast<int>(j) + 1);
        }
        doc["nodes"] = nodes;
        doc["skins"] = nlohmann::json::array(
            {{{"joints", joint_indices}, {"inverseBindMatrices", a_ibm}}});
        nlohmann::json scene_nodes = nlohmann::json::array({0});
        for (int r : roots) scene_nodes.push_back(r + 1);
        doc["scenes"] = nlohmann::json::array({{{"nodes", scene_nodes}}});
        doc["scene"] = 0;

        // Animations: animation channels target NODES, and the node tree is
        // copied verbatim, so the clips are valid for every LOD unchanged.
        // Clip NAMES are preserved: ISkeleton::play_clip matches on them.
        if (!skel->clips.empty()) {
            nlohmann::json animations = nlohmann::json::array();
            for (const auto& clip : skel->clips) {
                nlohmann::json samplers = nlohmann::json::array();
                nlohmann::json channels = nlohmann::json::array();
                for (const auto& ch : clip.channels) {
                    const int comps = (ch.path == gml::AnimPath::Rotation) ? 4 : 3;
                    const int tv = add_view(ch.times.data(), ch.times.size() * 4, 0);
                    const int vv = add_view(ch.values.data(), ch.values.size() * 4, 0);
                    const float tmin = ch.times.empty() ? 0.0f : ch.times.front();
                    const float tmax = ch.times.empty() ? 0.0f : ch.times.back();
                    const int in_acc = static_cast<int>(doc["accessors"].size());
                    // Sampler inputs REQUIRE min/max per the spec.
                    doc["accessors"].push_back(
                        {{"bufferView", tv},
                         {"componentType", 5126},
                         {"count", static_cast<std::uint64_t>(ch.times.size())},
                         {"type", "SCALAR"},
                         {"min", {tmin}},
                         {"max", {tmax}}});
                    const int out_acc = static_cast<int>(doc["accessors"].size());
                    doc["accessors"].push_back(
                        {{"bufferView", vv},
                         {"componentType", 5126},
                         {"count", static_cast<std::uint64_t>(
                                       ch.values.size() / static_cast<std::size_t>(comps))},
                         {"type", comps == 4 ? "VEC4" : "VEC3"}});
                    const char* interp = "LINEAR";
                    if (ch.interp == gml::AnimInterp::Step) interp = "STEP";
                    if (ch.interp == gml::AnimInterp::CubicSpline) interp = "CUBICSPLINE";
                    const char* p = "rotation";
                    if (ch.path == gml::AnimPath::Translation) p = "translation";
                    if (ch.path == gml::AnimPath::Scale) p = "scale";
                    const int sampler_index = static_cast<int>(samplers.size());
                    samplers.push_back(
                        {{"input", in_acc}, {"output", out_acc}, {"interpolation", interp}});
                    channels.push_back({{"sampler", sampler_index},
                                        {"target", {{"node", ch.joint + 1}, {"path", p}}}});
                }
                nlohmann::json anim{{"samplers", samplers}, {"channels", channels}};
                if (!clip.name.empty()) anim["name"] = clip.name;
                animations.push_back(anim);
            }
            doc["animations"] = animations;
        }
    }

    doc["buffers"] = nlohmann::json::array(
        {{{"byteLength", static_cast<std::uint64_t>(bin.size())}}});
    return write_glb_container(path, doc, bin);
}

// --- Modes -------------------------------------------------------------------

int list_images(const Options& opt) {
    std::vector<LitVertex> verts;
    std::vector<std::uint32_t> indices;
    MeshMaterials mats;
    gml::LoadOptions lo;
    lo.want_image_bytes = true;
    lo.bake_base_color_factor = false;
    std::string err;
    bool skinned = false;
    {
        bool has_skin = false;
        int jc = 0;
        std::string perr;
        skinned = gml::glb_probe_skin(opt.input, &has_skin, &jc, perr) && has_skin;
    }
    if (skinned) {
        std::vector<SkinnedVertex> sv;
        SkeletonData skel;
        if (!gml::load_glb_skinned(opt.input, 0.0f, sv, indices, skel, err, nullptr,
                                   &mats, lo)) {
            std::fprintf(stderr, "skinned load failed (%s): %s\n", opt.input.c_str(),
                         err.c_str());
            return 1;
        }
    } else if (!gml::load_glb_mesh(opt.input, 0.0f, verts, indices, &mats, err, 0.0f, lo)) {
        std::fprintf(stderr, "load failed (%s): %s\n", opt.input.c_str(), err.c_str());
        return 1;
    }
    fill_image_bytes(opt.input, mats);
    if (!opt.extract_dir.empty()) {
        std::filesystem::create_directories(opt.extract_dir);
    }

    std::vector<std::size_t> textured_tris(mats.images.size(), 0);
    std::size_t textured = 0;
    for (const auto& sm : mats.submeshes) {
        const int img = mats.materials[static_cast<std::size_t>(sm.material)].base_color_image;
        if (img >= 0) {
            textured_tris[static_cast<std::size_t>(img)] += sm.index_count / 3;
            textured += sm.index_count / 3;
        }
    }
    nlohmann::json out;
    out["input"] = opt.input;
    out["skinned"] = skinned;
    out["triangles"] = indices.size() / 3;
    out["textured_triangles"] = textured;
    out["missing_uvs"] = mats.missing_uvs;
    out["had_vertex_colors"] = mats.had_vertex_colors;
    int maps_normal = 0, maps_mr = 0, maps_occ = 0, maps_emis = 0;
    nlohmann::json mlist = nlohmann::json::array();
    for (std::size_t i = 0; i + 1 < mats.materials.size(); ++i) {
        const auto& m = mats.materials[i];
        maps_normal += m.has_normal_map;
        maps_mr += m.has_metallic_roughness_map;
        maps_occ += m.has_occlusion_map;
        maps_emis += m.has_emissive_map;
        mlist.push_back({{"name", m.name},
                         {"base_color_image", m.base_color_image},
                         {"factor", {m.base_color_factor[0], m.base_color_factor[1],
                                     m.base_color_factor[2], m.base_color_factor[3]}},
                         {"alpha_mode", static_cast<int>(m.alpha_mode)}});
    }
    out["materials"] = mlist;
    out["other_maps"] = {{"normal", maps_normal}, {"metallic_roughness", maps_mr},
                         {"occlusion", maps_occ}, {"emissive", maps_emis}};
    nlohmann::json ilist = nlohmann::json::array();
    for (std::size_t i = 0; i < mats.images.size(); ++i) {
        const auto& img = mats.images[i];
        int w = 0, h = 0, comp = 0;
        bool decodable = false;
        if (!img.bytes.empty()) {
            decodable = stbi_info_from_memory(img.bytes.data(),
                                              static_cast<int>(img.bytes.size()), &w, &h,
                                              &comp) != 0;
        }
        std::string file;
        if (!opt.extract_dir.empty() && !img.bytes.empty()) {
            file = (std::filesystem::path(opt.extract_dir) /
                    (img.hash + "." + ext_for_mime(img.mime)))
                       .string();
            if (!std::filesystem::exists(file)) {
                std::ofstream f(file, std::ios::binary);
                f.write(reinterpret_cast<const char*>(img.bytes.data()),
                        static_cast<std::streamsize>(img.bytes.size()));
            }
        }
        ilist.push_back({{"index", i},
                         {"hash", img.hash},
                         {"mime", img.mime},
                         {"ext", ext_for_mime(img.mime)},
                         {"bytes", img.bytes.size()},
                         {"width", w},
                         {"height", h},
                         {"channels", comp},
                         {"decodable", decodable},
                         {"name", img.name},
                         {"external_uri", img.uri},
                         {"textured_triangles", textured_tris[i]},
                         {"file", file}});
    }
    out["images"] = ilist;
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

template <class V>
void write_report(const Options& opt, const Optimized<V>& m, const WriteStats& st,
                  std::size_t source_tris, std::size_t source_verts,
                  const MeshMaterials& mats, std::size_t source_textured,
                  const SkeletonData* skel) {
    if (opt.report.empty()) return;
    const float extent[3] = {st.max_pos[0] - st.min_pos[0], st.max_pos[1] - st.min_pos[1],
                             st.max_pos[2] - st.min_pos[2]};
    float longest = std::max(extent[0], std::max(extent[1], extent[2]));
    const double radius =
        0.5 * std::sqrt(static_cast<double>(extent[0]) * extent[0] +
                        static_cast<double>(extent[1]) * extent[1] +
                        static_cast<double>(extent[2]) * extent[2]);
    nlohmann::json rep;
    rep["input"] = opt.input;
    rep["output"] = opt.output;
    rep["source_triangles"] = source_tris;
    rep["output_triangles"] = m.indices.size() / 3;
    rep["source_vertices"] = source_verts;
    rep["output_vertices"] = m.verts.size();
    rep["target_triangles"] = opt.target_tris;
    rep["aabb_min"] = {st.min_pos[0], st.min_pos[1], st.min_pos[2]};
    rep["aabb_max"] = {st.max_pos[0], st.max_pos[1], st.max_pos[2]};
    rep["longest_axis"] = longest;
    // Radius for a mesh normalised so its longest axis is 1 m; the stage
    // multiplies by the asset's target_scale_meters.
    rep["unit_bounds_radius"] = (longest > 1e-6f) ? (radius / longest) : 0.0;
    // meshopt's error is relative to the mesh extent; times the asset's size
    // in metres it is the largest geometric deviation of this LOD.
    rep["simplify_error_rel"] = m.simplify_error;
    rep["uv_weight"] = m.uv_weight;
    rep["used_sloppy"] = m.used_sloppy;
    rep["submeshes"] = m.submeshes.size();
    rep["materials"] = st.materials;
    rep["images"] = st.images;
    rep["source_textured_triangles"] = source_textured;
    rep["textured_triangles"] = st.textured_triangles;
    rep["source_missing_uvs"] = mats.missing_uvs;
    rep["vertex_colors"] = mats.had_vertex_colors;
    rep["skinned"] = skel != nullptr;
    rep["joint_count"] = skel ? skel->joint_count() : 0;
    rep["animation_clips"] = skel ? skel->clips.size() : 0;
    std::ofstream rf(opt.report);
    if (!rf) {
        std::fprintf(stderr, "cannot write report: %s\n", opt.report.c_str());
        return;
    }
    rf << rep.dump(2) << "\n";
}

std::size_t textured_triangle_count(const MeshMaterials& mats) {
    std::size_t n = 0;
    for (const auto& sm : mats.submeshes) {
        if (mats.materials[static_cast<std::size_t>(sm.material)].base_color_image >= 0) {
            n += sm.index_count / 3;
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage();
        return 2;
    }
    if (opt.list_images) return list_images(opt);

    std::unordered_map<std::string, TextureEntry> tmap;
    const std::unordered_map<std::string, TextureEntry>* tmap_ptr = nullptr;
    if (!opt.textures_map.empty()) {
        if (!load_texture_map(opt.textures_map, tmap)) return 1;
        tmap_ptr = &tmap;
    }

    gml::LoadOptions lo;
    lo.want_image_bytes = true;
    // Raw COLOR_0: the factor is written into the material, exactly as the
    // source had it, and the runtime multiplies the two back together.
    lo.bake_base_color_factor = false;

    bool has_skin = false;
    {
        int jc = 0;
        std::string perr;
        if (!gml::glb_probe_skin(opt.input, &has_skin, &jc, perr)) has_skin = false;
    }

    std::string err;
    MeshMaterials mats;
    if (has_skin) {
        std::vector<SkinnedVertex> verts;
        std::vector<std::uint32_t> indices;
        SkeletonData skel;
        if (!gml::load_glb_skinned(opt.input, 0.0f, verts, indices, skel, err, nullptr,
                                   &mats, lo)) {
            std::fprintf(stderr, "skinned load failed (%s): %s\n", opt.input.c_str(),
                         err.c_str());
            return 1;
        }
        fill_image_bytes(opt.input, mats);
        if (opt.allow_sloppy) {
            std::fprintf(stderr,
                         "note: --sloppy ignored for rigged input %s (sloppy "
                         "simplification does not preserve skin bindings)\n",
                         opt.input.c_str());
        }
        const std::size_t source_tris = indices.size() / 3;
        const std::size_t source_verts = verts.size();
        const std::size_t source_textured = textured_triangle_count(mats);
        auto tri_mat = triangle_materials(mats, source_tris);
        Optimized<SkinnedVertex> m;
        if (!optimize_mesh(std::move(verts), std::move(indices), std::move(tri_mat), mats,
                           opt.target_tris, /*skinned=*/true, false, m, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        WriteStats st;
        if (!write_glb_out(opt.output, m, mats, mats.had_vertex_colors, tmap_ptr, &skel, st)) {
            return 1;
        }
        std::printf("%s -> %s  tris %zu -> %zu  verts %zu -> %zu  joints %zu  "
                    "materials %zu  textured %zu/%zu (rigged)\n",
                    opt.input.c_str(), opt.output.c_str(), source_tris,
                    m.indices.size() / 3, source_verts, m.verts.size(), skel.joint_count(),
                    m.submeshes.size(), st.textured_triangles, m.indices.size() / 3);
        write_report(opt, m, st, source_tris, source_verts, mats, source_textured, &skel);
        return 0;
    }

    std::vector<LitVertex> verts;
    std::vector<std::uint32_t> indices;
    if (!gml::load_glb_mesh(opt.input, 0.0f, verts, indices, &mats, err, 0.0f, lo)) {
        std::fprintf(stderr, "load failed (%s): %s\n", opt.input.c_str(), err.c_str());
        return 1;
    }
    fill_image_bytes(opt.input, mats);
    auto tri_mat = triangle_materials(mats, indices.size() / 3);

    // Remove triangles socket_generation identified as a part the engine
    // replaces (triangle ids are load_glb_merged order).
    if (!opt.drop_triangles.empty()) {
        std::ifstream df(opt.drop_triangles);
        const nlohmann::json dj = nlohmann::json::parse(df, nullptr, false);
        if (dj.is_object() && dj.contains("triangles") && dj["triangles"].is_array()) {
            std::vector<char> drop(indices.size() / 3, 0);
            for (const auto& t : dj["triangles"]) {
                const auto id = t.get<std::int64_t>();
                if (id >= 0 && static_cast<std::size_t>(id) < drop.size()) drop[id] = 1;
            }
            std::vector<std::uint32_t> kept;
            std::vector<int> kept_mat;
            kept.reserve(indices.size());
            for (std::size_t t = 0; t < drop.size(); ++t) {
                if (drop[t]) continue;
                kept.insert(kept.end(), indices.begin() + static_cast<std::ptrdiff_t>(t * 3),
                            indices.begin() + static_cast<std::ptrdiff_t>(t * 3 + 3));
                kept_mat.push_back(tri_mat[t]);
            }
            std::fprintf(stderr, "dropped %zu of %zu triangles\n",
                         drop.size() - kept.size() / 3, drop.size());
            if (!kept.empty()) {
                indices.swap(kept);
                tri_mat.swap(kept_mat);
            }
        }
    }

    const std::size_t source_tris = indices.size() / 3;
    const std::size_t source_verts = verts.size();
    std::size_t source_textured = 0;
    for (std::size_t t = 0; t < tri_mat.size(); ++t) {
        if (mats.materials[static_cast<std::size_t>(tri_mat[t])].base_color_image >= 0) {
            ++source_textured;
        }
    }
    Optimized<LitVertex> m;
    if (!optimize_mesh(std::move(verts), std::move(indices), std::move(tri_mat), mats,
                       opt.target_tris, /*skinned=*/false, opt.allow_sloppy, m, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    WriteStats st;
    if (!write_glb_out(opt.output, m, mats, mats.had_vertex_colors, tmap_ptr,
                       static_cast<const SkeletonData*>(nullptr), st)) {
        return 1;
    }
    std::printf("%s -> %s  tris %zu -> %zu  verts %zu -> %zu  materials %zu  "
                "textured %zu/%zu\n",
                opt.input.c_str(), opt.output.c_str(), source_tris, m.indices.size() / 3,
                source_verts, m.verts.size(), m.submeshes.size(), st.textured_triangles,
                m.indices.size() / 3);
    write_report(opt, m, st, source_tris, source_verts, mats, source_textured,
                 static_cast<const SkeletonData*>(nullptr));
    return 0;
}
