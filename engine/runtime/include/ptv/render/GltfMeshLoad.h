#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ptv::gltf_mesh_load {

/** CPU-side static vertex. The GPU layout is packed separately (Renderer). */
struct LitVertex {
    float pos[3];
    float normal[3];
    /** Base-colour texture coordinate, KHR_texture_transform already applied. */
    float uv[2];
    // Per-vertex linear-space RGBA: COLOR_0 (white when absent), multiplied
    // by the material's baseColorFactor unless the caller asked for the raw
    // stream (LoadOptions::bake_base_color_factor = false).
    float color[4];
};

enum class AlphaMode : std::uint8_t { Opaque = 0, Mask = 1, Blend = 2 };

/** glTF sampler state (GL enums; -1 = unspecified). */
struct SamplerInfo {
    int mag_filter{-1};
    int min_filter{-1};
    int wrap_s{10497};  // REPEAT
    int wrap_t{10497};
};

/** One glTF material, reduced to what the base-colour renderer uses. The
 *  normal / metallic-roughness / occlusion / emissive maps are counted, not
 *  loaded, so reports can say what stayed on disk. */
struct MaterialInfo {
    std::string name;
    float base_color_factor[4]{1.0f, 1.0f, 1.0f, 1.0f};
    /** Index into MeshMaterials::images, or -1 for an untextured material. */
    int base_color_image{-1};
    /** TEXCOORD set the base-colour texture reads. */
    int base_color_texcoord{0};
    SamplerInfo sampler{};
    AlphaMode alpha_mode{AlphaMode::Opaque};
    float alpha_cutoff{0.5f};
    bool double_sided{false};
    bool has_normal_map{false};
    bool has_metallic_roughness_map{false};
    bool has_occlusion_map{false};
    bool has_emissive_map{false};
};

/** One glTF image as referenced by a base-colour texture. */
struct ImageInfo {
    std::string name;
    std::string mime;
    /** External uri exactly as written in the GLB (relative to the GLB's
     *  directory), empty for an embedded image. */
    std::string uri;
    /** ASTC KTX2 twin from the ``PTV_texture_astc`` texture extension,
     *  relative to the GLB's directory; empty when there is none. */
    std::string ktx2_uri;
    /** Encoded bytes of an embedded image (bufferView or data uri). Filled
     *  only when LoadOptions::want_image_bytes is set. */
    std::vector<unsigned char> bytes;
    /** FNV-1a 64 of the encoded bytes (hex), when the bytes were read. */
    std::string hash;
};

/** A contiguous index range drawn with one material. */
struct SubMesh {
    std::uint32_t first_index{0};
    std::uint32_t index_count{0};
    int material{-1};
};

struct MeshMaterials {
    std::vector<MaterialInfo> materials;
    std::vector<ImageInfo> images;
    /** In index-buffer order; together they cover every index exactly once. */
    std::vector<SubMesh> submeshes;
    /** True when any primitive carried COLOR_0. */
    bool had_vertex_colors{false};
    /** True when any textured primitive lacked the TEXCOORD it needs. */
    bool missing_uvs{false};
};

struct LoadOptions {
    /** Multiply baseColorFactor into the vertex colour (the runtime path). */
    bool bake_base_color_factor{true};
    /** Copy embedded image bytes into ImageInfo::bytes (tools, fallbacks). */
    bool want_image_bytes{false};
};

/** FNV-1a 64-bit hash of a byte range as 16 lowercase hex digits. Names the
 *  shared texture files, so the same image is stored and uploaded once. */
std::string content_hash_hex(const unsigned char* data, std::size_t size);

/** ``load_glb_merged`` plus materials, per-primitive submeshes, UVs and the
 *  base-colour image references. Triangle order is identical to
 *  ``load_glb_merged`` (socket triangle ids depend on it). */
bool load_glb_mesh(const std::string& path,
                   float target_scale_meters,
                   std::vector<LitVertex>& out_verts,
                   std::vector<uint32_t>& out_indices,
                   MeshMaterials* out_materials,
                   std::string& err,
                   float unit_scale = 0.0f,
                   const LoadOptions& options = {});

/** Resolve ``uri`` (relative, may contain ``..``) against the directory of
 *  ``glb_path`` into a normalised forward-slash path the asset source can
 *  open (filesystem or APK). */
std::string resolve_relative_uri(const std::string& glb_path,
                                 const std::string& uri);

/** Merge all triangle meshes in a GLB, rescale to ``target_scale_meters``
 *  on the longest axis of the bounding box, and return interleaved vertices
 *  + uint32 indices.
 *
 *  ``unit_scale`` > 0 overrides that: positions are multiplied by exactly
 *  this factor (metres per source unit) instead of being re-measured. The
 *  asset optimizer computes it once from LOD0 and the scene compiler writes
 *  it for every LOD, so all levels of one asset come out the same size even
 *  if simplification moved a few vertices of a coarse level. */
bool load_glb_merged(const std::string& path,
                     float target_scale_meters,
                     std::vector<LitVertex>& out_verts,
                     std::vector<uint32_t>& out_indices,
                     std::string& err,
                     float unit_scale = 0.0f);

struct TexturedVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
};

/** A base-colour image exactly as stored in the GLB (PNG / JPEG bytes). */
struct EncodedImage {
    std::string mime;
    std::vector<unsigned char> bytes;
};

struct TexturedMesh {
    std::vector<TexturedVertex> verts;
    std::vector<uint32_t> indices;
    /** Per triangle: index into ``images`` of its base-colour texture, or -1. */
    std::vector<int> tri_image;
    std::vector<EncodedImage> images;
};

/** The same merge as ``load_glb_merged`` at source scale (identical node
 *  traversal and therefore identical positions, triangle for triangle), plus
 *  TEXCOORD_0 and each primitive's encoded base-colour image. Offline tools
 *  only (the socket-view renderer); the runtime never decodes textures. */
bool load_glb_textured(const std::string& path, TexturedMesh& out,
                       std::string& err);

}  // namespace ptv::gltf_mesh_load
