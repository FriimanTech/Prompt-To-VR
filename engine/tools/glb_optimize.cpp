// ptv_glb_optimize: offline GLB decimation + LOD generation.
//
// Used by the orchestrator's ``asset_optimization`` stage to fit generated
// (Tripo) assets into the per-project triangle budget before they ever reach
// the headset. Raw Tripo output is routinely ~250k verts / ~500k triangles per
// asset, which blows straight past Meta's Quest 3 guidance of 1.3M-1.8M
// triangles for the WHOLE frame.
//
// Pipeline per invocation:
//   1. Load + merge the source GLB with the SAME loader the runtime uses
//      (ptv::gltf_mesh_load::load_glb_merged), so the geometry, the baked
//      baseColorFactor/COLOR_0 vertex colours, and the primitive merging all
//      match what the engine would have produced at load time.
//      Passing target_scale_meters = 0 keeps the source coordinate system;
//      the runtime re-normalises by AABB at load, so scale round-trips.
//   2. Weld duplicate vertices, then simplify to the requested triangle count
//      with meshoptimizer (falling back to the sloppy simplifier when the
//      error-bounded pass cannot reach an aggressive target).
//   3. Optimise for vertex cache / overdraw / vertex fetch.
//   4. Write a single-primitive GLB (POSITION + NORMAL + COLOR_0, white
//      baseColorFactor so the runtime's ``COLOR_0 * baseColorFactor`` bake is
//      an exact round-trip).
//
// Images/textures are intentionally NOT copied into the output: the forward
// renderer does not sample them today, and dropping them is what takes a
// ~110 MB APK back down to something that streams quickly. The untouched
// source GLB stays on disk, so re-running with textures remains possible if
// PBR lands later.
//
// The GLB container is written by hand (nlohmann::json + the binary chunk)
// rather than through tinygltf's writer: the runtime already compiles
// TINYGLTF_IMPLEMENTATION, so pulling the writer in here would risk ODR
// clashes, and we want byte-exact control over the layout we emit.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#include "ptv/render/GltfMeshLoad.h"

namespace {

using ptv::gltf_mesh_load::LitVertex;

struct Options {
    std::string input;
    std::string output;
    std::string report;
    std::size_t target_tris = 0;
    bool allow_sloppy = false;
};

void print_usage() {
    std::fprintf(
        stderr,
        "usage: ptv_glb_optimize --input <in.glb> --output <out.glb>\n"
        "                       --target-tris <N> [--sloppy]\n"
        "                       [--report <report.json>]\n");
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
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return !opt.input.empty() && !opt.output.empty() && opt.target_tris > 0;
}

// --- GLB writing -----------------------------------------------------------

void append_padded(std::vector<std::uint8_t>& bin, const void* data,
                   std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    bin.insert(bin.end(), p, p + bytes);
    while (bin.size() % 4 != 0) bin.push_back(0);
}

bool write_glb(const std::string& path, const std::vector<LitVertex>& verts,
               const std::vector<std::uint32_t>& indices,
               const float min_pos[3], const float max_pos[3]) {
    // Interleaved LitVertex is not directly usable as glTF accessors without
    // strides, so de-interleave into three tightly packed streams. Simpler to
    // read back and lets each accessor use its natural alignment.
    std::vector<float> positions(verts.size() * 3);
    std::vector<float> normals(verts.size() * 3);
    std::vector<float> colors(verts.size() * 4);
    for (std::size_t i = 0; i < verts.size(); ++i) {
        std::memcpy(&positions[i * 3], verts[i].pos, sizeof(float) * 3);
        std::memcpy(&normals[i * 3], verts[i].normal, sizeof(float) * 3);
        std::memcpy(&colors[i * 4], verts[i].color, sizeof(float) * 4);
    }

    std::vector<std::uint8_t> bin;
    const std::size_t idx_off = 0;
    append_padded(bin, indices.data(), indices.size() * sizeof(std::uint32_t));
    const std::size_t pos_off = bin.size();
    append_padded(bin, positions.data(), positions.size() * sizeof(float));
    const std::size_t nrm_off = bin.size();
    append_padded(bin, normals.data(), normals.size() * sizeof(float));
    const std::size_t col_off = bin.size();
    append_padded(bin, colors.data(), colors.size() * sizeof(float));

    nlohmann::json doc;
    doc["asset"] = {{"version", "2.0"},
                    {"generator", "ptv_glb_optimize"}};
    doc["buffers"] = nlohmann::json::array(
        {{{"byteLength", static_cast<std::uint64_t>(bin.size())}}});

    // 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER
    doc["bufferViews"] = nlohmann::json::array({
        {{"buffer", 0},
         {"byteOffset", static_cast<std::uint64_t>(idx_off)},
         {"byteLength",
          static_cast<std::uint64_t>(indices.size() * sizeof(std::uint32_t))},
         {"target", 34963}},
        {{"buffer", 0},
         {"byteOffset", static_cast<std::uint64_t>(pos_off)},
         {"byteLength",
          static_cast<std::uint64_t>(positions.size() * sizeof(float))},
         {"target", 34962}},
        {{"buffer", 0},
         {"byteOffset", static_cast<std::uint64_t>(nrm_off)},
         {"byteLength",
          static_cast<std::uint64_t>(normals.size() * sizeof(float))},
         {"target", 34962}},
        {{"buffer", 0},
         {"byteOffset", static_cast<std::uint64_t>(col_off)},
         {"byteLength",
          static_cast<std::uint64_t>(colors.size() * sizeof(float))},
         {"target", 34962}},
    });

    // 5125 = UNSIGNED_INT, 5126 = FLOAT. POSITION min/max are required by the
    // glTF 2.0 spec.
    doc["accessors"] = nlohmann::json::array({
        {{"bufferView", 0},
         {"componentType", 5125},
         {"count", static_cast<std::uint64_t>(indices.size())},
         {"type", "SCALAR"}},
        {{"bufferView", 1},
         {"componentType", 5126},
         {"count", static_cast<std::uint64_t>(verts.size())},
         {"type", "VEC3"},
         {"min", {min_pos[0], min_pos[1], min_pos[2]}},
         {"max", {max_pos[0], max_pos[1], max_pos[2]}}},
        {{"bufferView", 2},
         {"componentType", 5126},
         {"count", static_cast<std::uint64_t>(verts.size())},
         {"type", "VEC3"}},
        {{"bufferView", 3},
         {"componentType", 5126},
         {"count", static_cast<std::uint64_t>(verts.size())},
         {"type", "VEC4"}},
    });

    // White baseColorFactor: the per-vertex colour already carries the source
    // material colour, and the runtime multiplies the two.
    doc["materials"] = nlohmann::json::array({{
        {"name", "ptv_baked_vertex_color"},
        {"pbrMetallicRoughness",
         {{"baseColorFactor", {1.0, 1.0, 1.0, 1.0}},
          {"metallicFactor", 0.0},
          {"roughnessFactor", 1.0}}},
        {"doubleSided", true},
    }});

    doc["meshes"] = nlohmann::json::array({{
        {"primitives",
         nlohmann::json::array({{
             {"attributes",
              {{"POSITION", 1}, {"NORMAL", 2}, {"COLOR_0", 3}}},
             {"indices", 0},
             {"material", 0},
             {"mode", 4},
         }})},
    }});
    doc["nodes"] = nlohmann::json::array({{{"mesh", 0}}});
    doc["scenes"] = nlohmann::json::array({{{"nodes", {0}}}});
    doc["scene"] = 0;

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

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage();
        return 2;
    }

    // Step 1: load + merge exactly like the runtime does. target_scale_meters
    // of 0 disables the AABB re-normalisation so we keep source coordinates.
    std::vector<LitVertex> verts;
    std::vector<std::uint32_t> indices;
    std::string err;
    if (!ptv::gltf_mesh_load::load_glb_merged(opt.input, 0.0f, verts, indices,
                                              err)) {
        std::fprintf(stderr, "load failed (%s): %s\n", opt.input.c_str(),
                     err.c_str());
        return 1;
    }

    const std::size_t source_tris = indices.size() / 3;

    // Step 2: weld duplicate vertices so the simplifier can collapse across
    // former primitive seams.
    std::vector<unsigned int> remap(indices.size());
    const std::size_t unique_verts = meshopt_generateVertexRemap(
        remap.data(), indices.data(), indices.size(), verts.data(),
        verts.size(), sizeof(LitVertex));

    std::vector<LitVertex> welded(unique_verts);
    std::vector<unsigned int> welded_idx(indices.size());
    meshopt_remapVertexBuffer(welded.data(), verts.data(), verts.size(),
                              sizeof(LitVertex), remap.data());
    meshopt_remapIndexBuffer(welded_idx.data(), indices.data(), indices.size(),
                             remap.data());

    // Step 3: simplify toward the requested triangle budget.
    const std::size_t target_index_count = opt.target_tris * 3;
    std::vector<unsigned int> simplified(welded_idx.size());
    std::size_t result_count = welded_idx.size();

    if (target_index_count < welded_idx.size()) {
        float result_error = 0.0f;
        // A generous target_error lets the simplifier actually reach very
        // aggressive budgets; quality is governed by the budget itself.
        result_count = meshopt_simplify(
            simplified.data(), welded_idx.data(), welded_idx.size(),
            &welded[0].pos[0], welded.size(), sizeof(LitVertex),
            target_index_count, /*target_error=*/1.0f, /*options=*/0,
            &result_error);

        // The error-bounded simplifier preserves topology and can stall well
        // above the target on shells with lots of boundaries. Fall back to the
        // sloppy simplifier for the aggressive LODs where silhouette accuracy
        // matters less than hitting the budget.
        if (opt.allow_sloppy && result_count > target_index_count * 3 / 2) {
            std::vector<unsigned int> sloppy(welded_idx.size());
            float sloppy_error = 0.0f;
            const std::size_t sloppy_count = meshopt_simplifySloppy(
                sloppy.data(), welded_idx.data(), welded_idx.size(),
                &welded[0].pos[0], welded.size(), sizeof(LitVertex),
                target_index_count, /*target_error=*/1.0f, &sloppy_error);
            if (sloppy_count >= 3 && sloppy_count < result_count) {
                simplified = std::move(sloppy);
                result_count = sloppy_count;
            }
        }
        simplified.resize(result_count);
    } else {
        // Already under budget: keep every triangle (never upsample).
        simplified = welded_idx;
        result_count = welded_idx.size();
    }

    if (result_count < 3) {
        std::fprintf(stderr, "simplification produced no triangles\n");
        return 1;
    }

    // Step 4: GPU-friendly ordering, then compact the vertex buffer down to
    // only the vertices the simplified index buffer still references.
    meshopt_optimizeVertexCache(simplified.data(), simplified.data(),
                                result_count, welded.size());
    meshopt_optimizeOverdraw(simplified.data(), simplified.data(), result_count,
                             &welded[0].pos[0], welded.size(),
                             sizeof(LitVertex), 1.05f);

    std::vector<LitVertex> final_verts(welded.size());
    const std::size_t final_vert_count = meshopt_optimizeVertexFetch(
        final_verts.data(), simplified.data(), result_count, welded.data(),
        welded.size(), sizeof(LitVertex));
    final_verts.resize(final_vert_count);

    std::vector<std::uint32_t> final_indices(simplified.begin(),
                                             simplified.end());

    // Bounds of the optimised mesh, in source units. The runtime uses the same
    // longest-axis convention to rescale to target_scale_meters, and the
    // renderer needs the radius for frustum / distance culling.
    float min_pos[3] = {std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max()};
    float max_pos[3] = {std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest()};
    for (const auto& v : final_verts) {
        for (int c = 0; c < 3; ++c) {
            if (v.pos[c] < min_pos[c]) min_pos[c] = v.pos[c];
            if (v.pos[c] > max_pos[c]) max_pos[c] = v.pos[c];
        }
    }

    if (!write_glb(opt.output, final_verts, final_indices, min_pos, max_pos)) {
        return 1;
    }

    const std::size_t out_tris = final_indices.size() / 3;
    std::printf("%s -> %s  tris %zu -> %zu  verts %zu -> %zu\n",
                opt.input.c_str(), opt.output.c_str(), source_tris, out_tris,
                verts.size(), final_verts.size());

    if (!opt.report.empty()) {
        // Longest-axis extent matches how the runtime normalises scale, and the
        // half-diagonal is the local bounding-sphere radius used for culling.
        float extent[3] = {max_pos[0] - min_pos[0], max_pos[1] - min_pos[1],
                           max_pos[2] - min_pos[2]};
        float longest = extent[0];
        if (extent[1] > longest) longest = extent[1];
        if (extent[2] > longest) longest = extent[2];
        const double radius =
            0.5 * std::sqrt(static_cast<double>(extent[0]) * extent[0] +
                            static_cast<double>(extent[1]) * extent[1] +
                            static_cast<double>(extent[2]) * extent[2]);

        nlohmann::json rep;
        rep["input"] = opt.input;
        rep["output"] = opt.output;
        rep["source_triangles"] = source_tris;
        rep["output_triangles"] = out_tris;
        rep["source_vertices"] = verts.size();
        rep["output_vertices"] = final_verts.size();
        rep["target_triangles"] = opt.target_tris;
        rep["aabb_min"] = {min_pos[0], min_pos[1], min_pos[2]};
        rep["aabb_max"] = {max_pos[0], max_pos[1], max_pos[2]};
        rep["longest_axis"] = longest;
        // Radius for a mesh normalised so its longest axis is 1 m; the stage
        // multiplies by the asset's target_scale_meters.
        rep["unit_bounds_radius"] = (longest > 1e-6f) ? (radius / longest) : 0.0;

        std::ofstream rf(opt.report);
        if (!rf) {
            std::fprintf(stderr, "cannot write report: %s\n",
                         opt.report.c_str());
            return 1;
        }
        rf << rep.dump(2) << "\n";
    }
    return 0;
}
