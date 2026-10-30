#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ptv::gltf_mesh_load {

struct LitVertex {
    float pos[3];
    float normal[3];
    // Per-vertex linear-space RGBA. Filled from the source primitive's
    // material baseColorFactor (or per-vertex COLOR_0 if present) so each
    // Tripo / generated GLB renders with its design color, not a flat tint.
    // Defaults to white so the cube path can share the same vertex layout.
    float color[4];
};

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
