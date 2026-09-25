// ptv_glb_sockets: offline, deterministic socket derivation for a GLB.
//
// Used by the orchestrator's ``asset_contract`` stage. A socket is a NAMED
// LOCAL TRANSFORM on an asset -- "the muzzle is here, pointing that way" --
// and it is what lets generated C++ spawn a projectile at the barrel tip of a
// pistol, a rifle or a rocket launcher with the SAME weapon-agnostic code:
//
//     const Mat4 m = ctx.sockets->socket_world(self, "muzzle");
//
// The code-generation agent never sees the mesh, so it can never know where
// any particular barrel ends. That is deliberate: the agent writes logic
// against a named contract, and THIS tool -- which does have the geometry
// open -- produces the numbers behind that contract.
//
// Why C++ rather than Python: finding a muzzle needs real vertex positions,
// and vertices live in the GLB's binary chunk behind accessors and buffer
// views. The orchestrator's Python only ever parses the GLB header + JSON
// chunk (see stage_asset_normalize) and deliberately takes no glTF library
// dependency. The engine already has a battle-tested decoder, so we reuse it:
// ``load_glb_merged`` with target_scale_meters = 0 keeps the source
// coordinate system exactly as the runtime would load it.
//
// Everything here is deterministic -- no model calls, no randomness. The same
// GLB always yields the same sockets, so the contract is cacheable and a
// pipeline re-run never silently moves a muzzle.
//
// Output positions are NORMALIZED by the longest-axis extent, matching the
// ``unit_bounds_radius`` convention ptv_glb_optimize already emits. The scene
// compiler multiplies by the asset's ``target_scale_meters`` to get metres,
// exactly as it already does for ``bounds_radius_m``. Normalizing here is what
// makes a socket survive the LOD chain: decimation shifts the AABB slightly,
// and a unit-space socket is immune to that drift.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "ptv/render/GltfMeshLoad.h"

namespace {

using ptv::gltf_mesh_load::LitVertex;

struct Options {
    std::string input;
    std::string output;
    // Fraction of vertices at the muzzle end averaged to place the socket.
    // A single extreme vertex is noise; a few percent is the barrel rim.
    float tip_fraction = 0.02f;
};

void print_usage() {
    std::fprintf(stderr,
                 "usage: ptv_glb_sockets --input <in.glb> --output <out.json>\n"
                 "                      [--tip-fraction <0..1>]\n");
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
        } else if (a == "--tip-fraction") {
            std::string v;
            if (!next(v)) return false;
            opt.tip_fraction = std::clamp(std::stof(v), 0.001f, 0.5f);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return !opt.input.empty() && !opt.output.empty();
}

// --- Symmetric 3x3 eigendecomposition (cyclic Jacobi) -----------------------
//
// Small, exact enough, and fully deterministic -- no dependence on iteration
// order or floating-point reassociation across compilers, which matters
// because the resulting socket lands in a committed artifact.

struct Eigen3 {
    glm::vec3 values{0.0f};    // descending by magnitude
    glm::mat3 vectors{1.0f};   // column i is the eigenvector for values[i]
};

Eigen3 jacobi_eigen(glm::mat3 a) {
    glm::mat3 v(1.0f);
    for (int sweep = 0; sweep < 24; ++sweep) {
        // Largest off-diagonal magnitude; stop once the matrix is diagonal.
        float off = 0.0f;
        for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 3; ++j) off += a[j][i] * a[j][i];
        }
        if (off < 1e-20f) break;

        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                const float apq = a[q][p];
                if (std::fabs(apq) < 1e-20f) continue;
                const float theta = (a[q][q] - a[p][p]) / (2.0f * apq);
                const float t =
                    (theta >= 0.0f ? 1.0f : -1.0f) /
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

/** Rotation taking +Z onto ``forward``, with ``up_hint`` resolving the roll.
 *
 *  +Z is the socket convention: a mechanic reads the third basis column of
 *  ``socket_world`` as "the direction this socket faces". */
glm::quat rotation_from_forward(const glm::vec3& forward,
                                const glm::vec3& up_hint) {
    const glm::vec3 f = glm::normalize(forward);
    glm::vec3 up = up_hint;
    // Degenerate when the hint is parallel to forward; any perpendicular does.
    if (std::fabs(glm::dot(f, glm::normalize(up))) > 0.999f) {
        up = std::fabs(f.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    }
    const glm::vec3 right = glm::normalize(glm::cross(up, f));
    const glm::vec3 ortho_up = glm::cross(f, right);
    glm::mat3 basis(right, ortho_up, f);
    return glm::normalize(glm::quat_cast(basis));
}

nlohmann::json transform_json(const glm::vec3& p, const glm::quat& q) {
    // Rotation is [x, y, z, w] to match the scene.json convention that
    // SceneLoader::read_quat already parses.
    return nlohmann::json{{"position", {p.x, p.y, p.z}},
                          {"rotation", {q.x, q.y, q.z, q.w}}};
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage();
        return 2;
    }

    std::vector<LitVertex> verts;
    std::vector<std::uint32_t> indices;
    std::string err;
    // Scale 0 = keep the source coordinate system; we normalize ourselves.
    if (!ptv::gltf_mesh_load::load_glb_merged(opt.input, 0.0f, verts, indices,
                                              err)) {
        std::fprintf(stderr, "load failed (%s): %s\n", opt.input.c_str(),
                     err.c_str());
        return 1;
    }
    if (verts.empty()) {
        std::fprintf(stderr, "no vertices in %s\n", opt.input.c_str());
        return 1;
    }

    // --- Bounds + centroid --------------------------------------------------
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    glm::dvec3 sum(0.0);
    for (const auto& v : verts) {
        const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
        sum += glm::dvec3(p);
    }
    const glm::vec3 centroid(sum / static_cast<double>(verts.size()));
    const glm::vec3 extent = hi - lo;
    const float longest = std::max({extent.x, extent.y, extent.z});
    if (longest < 1e-6f) {
        std::fprintf(stderr, "degenerate bounds in %s\n", opt.input.c_str());
        return 1;
    }

    // --- Principal axis (PCA) ----------------------------------------------
    // The dominant eigenvector of the vertex covariance is the direction the
    // object is longest in. For essentially every firearm that is the barrel
    // axis, which is why this generalizes across a pistol, a rifle and a
    // rocket launcher without any per-weapon tuning.
    glm::mat3 cov(0.0f);
    for (const auto& v : verts) {
        const glm::vec3 d = glm::vec3(v.pos[0], v.pos[1], v.pos[2]) - centroid;
        cov[0][0] += d.x * d.x;
        cov[1][1] += d.y * d.y;
        cov[2][2] += d.z * d.z;
        cov[1][0] += d.x * d.y;
        cov[2][0] += d.x * d.z;
        cov[2][1] += d.y * d.z;
    }
    const float inv_n = 1.0f / static_cast<float>(verts.size());
    cov[0][0] *= inv_n; cov[1][1] *= inv_n; cov[2][2] *= inv_n;
    cov[1][0] *= inv_n; cov[2][0] *= inv_n; cov[2][1] *= inv_n;
    cov[0][1] = cov[1][0];
    cov[0][2] = cov[2][0];
    cov[1][2] = cov[2][1];

    const Eigen3 eig = jacobi_eigen(cov);
    glm::vec3 axis = eig.vectors[0];        // longest
    const glm::vec3 secondary = eig.vectors[1];

    // --- Which end is the muzzle? ------------------------------------------
    // Mass asymmetry. Split the vertices at the centroid along the principal
    // axis and measure how far each half sprawls along the SECONDARY axis. A
    // weapon's grip end has a handle, magazine and stock hanging off to the
    // side; the muzzle end is a comparatively bare tube. The heavier half is
    // therefore the grip, and the muzzle is the opposite extreme.
    double sprawl[2] = {0.0, 0.0};
    std::size_t count[2] = {0, 0};
    for (const auto& v : verts) {
        const glm::vec3 d = glm::vec3(v.pos[0], v.pos[1], v.pos[2]) - centroid;
        const int half = glm::dot(d, axis) >= 0.0f ? 1 : 0;
        sprawl[half] += std::fabs(glm::dot(d, secondary));
        ++count[half];
    }
    // Normalize by population so a lopsided vertex distribution (dense
    // detailing on one end) does not by itself decide the answer.
    const double mean_sprawl_neg =
        count[0] ? sprawl[0] / static_cast<double>(count[0]) : 0.0;
    const double mean_sprawl_pos =
        count[1] ? sprawl[1] / static_cast<double>(count[1]) : 0.0;

    // Point ``axis`` from the grip toward the muzzle.
    const bool grip_is_positive = mean_sprawl_pos > mean_sprawl_neg;
    if (grip_is_positive) axis = -axis;

    // --- Refine the barrel direction ----------------------------------------
    // PCA over a whole pistol averages the barrel with the handle hanging off
    // it, so the principal axis comes out tilted ~20 degrees from the barrel.
    // The MUZZLE half is a comparatively bare slide or tube, so its own
    // principal axis is the barrel line. (Snapping to model axes does not
    // work: generated GLBs often carry a node rotation, so the barrel is not
    // aligned with any model axis at all.)
    {
        glm::dvec3 fsum(0.0);
        std::size_t fn = 0;
        for (const auto& v : verts) {
            const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
            if (glm::dot(p - centroid, axis) > 0.0f) {
                fsum += glm::dvec3(p);
                ++fn;
            }
        }
        if (fn >= 16) {
            const glm::vec3 fc(fsum / static_cast<double>(fn));
            glm::mat3 fcov(0.0f);
            for (const auto& v : verts) {
                const glm::vec3 p(v.pos[0], v.pos[1], v.pos[2]);
                if (glm::dot(p - centroid, axis) <= 0.0f) continue;
                const glm::vec3 d = p - fc;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) fcov[c][r] += d[r] * d[c];
                }
            }
            const Eigen3 fe = jacobi_eigen(fcov);
            glm::vec3 barrel = fe.vectors[0];
            if (glm::dot(barrel, axis) < 0.0f) barrel = -barrel;
            // Only trust it when the front half really is elongated along
            // something close to the whole-object axis.
            if (fe.values[0] > 2.0f * fe.values[1] &&
                glm::dot(barrel, axis) > std::cos(glm::radians(35.0f))) {
                axis = glm::normalize(barrel);
            }
        }
    }

    auto snap_to_axis = [](const glm::vec3& v, float max_angle_cos) {
        const glm::vec3 axes[3] = {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                   glm::vec3(0, 0, 1)};
        glm::vec3 best = v;
        float best_dot = max_angle_cos;
        for (const auto& a : axes) {
            const float d = glm::dot(v, a);
            if (std::fabs(d) > best_dot) {
                best_dot = std::fabs(d);
                best = d > 0.0f ? a : -a;
            }
        }
        return best;
    };

    // --- Which way is up? ----------------------------------------------------
    // A weapon's handle, magazine and trigger guard hang BELOW the barrel, so
    // the grip half's mass sits off the barrel line on the underside. That
    // is a far better "up" than assuming the model's +Y -- a model exported
    // lying on its side still gets an upright hold.
    glm::vec3 up_hint(0.0f, 1.0f, 0.0f);
    {
        glm::dvec3 below(0.0);
        std::size_t n_below = 0;
        for (const auto& v : verts) {
            const glm::vec3 d =
                glm::vec3(v.pos[0], v.pos[1], v.pos[2]) - centroid;
            if (glm::dot(d, axis) >= 0.0f) continue;  // grip half only
            below += glm::dvec3(d - axis * glm::dot(d, axis));
            ++n_below;
        }
        if (n_below > 0) {
            const glm::vec3 off(below / static_cast<double>(n_below));
            if (glm::length(off) > 0.02f * longest) {
                glm::vec3 up = -glm::normalize(off);
                up = glm::normalize(
                    snap_to_axis(up, std::cos(glm::radians(30.0f))));
                if (std::fabs(glm::dot(up, axis)) < 0.5f) up_hint = up;
            }
        }
    }

    // --- Place the sockets --------------------------------------------------
    // Muzzle: mean of the outermost slice of vertices along +axis. Averaging a
    // slice rather than taking the single farthest vertex keeps a stray spike
    // (an antenna, a sight post) from dragging the muzzle off the barrel.
    std::vector<float> projections;
    projections.reserve(verts.size());
    for (const auto& v : verts) {
        projections.push_back(
            glm::dot(glm::vec3(v.pos[0], v.pos[1], v.pos[2]) - centroid, axis));
    }
    std::vector<float> sorted = projections;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t tip_n = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<float>(sorted.size()) *
                                    opt.tip_fraction));
    const float tip_cutoff = sorted[sorted.size() - tip_n];

    glm::dvec3 tip_sum(0.0);
    std::size_t tip_count = 0;
    glm::dvec3 grip_sum(0.0);
    std::size_t grip_count = 0;
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const glm::vec3 p(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        if (projections[i] >= tip_cutoff) {
            tip_sum += glm::dvec3(p);
            ++tip_count;
        }
        if (projections[i] < 0.0f) {  // grip half, behind the centroid
            grip_sum += glm::dvec3(p);
            ++grip_count;
        }
    }
    const glm::vec3 muzzle_pos(tip_sum / static_cast<double>(tip_count));
    const glm::vec3 grip_pos =
        grip_count ? glm::vec3(grip_sum / static_cast<double>(grip_count))
                   : centroid;

    // --- Emit ---------------------------------------------------------------
    // Unit space: divide by the longest-axis extent. The scene compiler
    // multiplies by target_scale_meters, mirroring bounds_radius_m.
    const float inv_longest = 1.0f / longest;
    const glm::quat forward_rot = rotation_from_forward(axis, up_hint);

    nlohmann::json doc;
    doc["input"] = opt.input;
    doc["source_vertices"] = verts.size();
    doc["source_triangles"] = indices.size() / 3;
    doc["aabb_min"] = {lo.x, lo.y, lo.z};
    doc["aabb_max"] = {hi.x, hi.y, hi.z};
    doc["longest_axis"] = longest;
    doc["principal_axis"] = {axis.x, axis.y, axis.z};

    // Positions stay relative to the MESH ORIGIN, not to the AABB corner: the
    // runtime loader only ever scales positions about the origin and never
    // recenters them, so origin-relative unit coordinates round-trip exactly
    // through both the LOD chain and target_scale_meters.
    nlohmann::json sockets;
    sockets["muzzle"] = transform_json(muzzle_pos * inv_longest, forward_rot);
    sockets["grip"] = transform_json(grip_pos * inv_longest, forward_rot);
    doc["sockets"] = sockets;

    // Every socket here came from geometry, never from a model. Recorded so a
    // later vision-assisted pass can be told apart in the artifact.
    doc["socket_confidence"] = nlohmann::json{{"muzzle", "heuristic"},
                                              {"grip", "heuristic"}};

    std::ofstream out(opt.output, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", opt.output.c_str());
        return 1;
    }
    out << doc.dump(2) << "\n";
    return 0;
}
