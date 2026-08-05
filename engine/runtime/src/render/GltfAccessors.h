#pragma once

// Internal glTF accessor helpers shared by the static mesh loader
// (GltfMeshLoad.cpp) and the skinned mesh / skeleton loader
// (GltfSkinnedLoad.cpp).
//
// This header lives under src/ on purpose: tinygltf types leak through the
// signatures, and nothing outside the render layer should depend on them.
// The public surface stays GltfMeshLoad.h / GltfSkinnedLoad.h.
//
// Exactly ONE translation unit may define TINYGLTF_IMPLEMENTATION before
// including this header; GltfMeshLoad.cpp is that unit.
//
// The TINY_GLTF_H_ probe below is load-bearing, not belt-and-braces:
// tiny_gltf.h puts its `#if defined(TINYGLTF_IMPLEMENTATION)` block OUTSIDE
// its own include guard, so a second `#include <tiny_gltf.h>` in the
// implementation TU expands every function body a second time and the
// translation unit dies in a wall of C2084 redefinitions. When the header is
// already in scope we must not pull it in again.

#ifndef TINY_GLTF_H_
#ifndef TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE
#endif
#ifndef TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#endif
#include <tiny_gltf.h>
#endif  // TINY_GLTF_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace ptv::gltf_accessors {

// No-op image loader: tinygltf calls this once per ``image`` in the GLB.
// We never sample textures, so we tell tinygltf "the image is fine" without
// decoding pixels. With TINYGLTF_NO_STB_IMAGE set, tinygltf hard-fails with
// "No LoadImageData callback specified." unless a callback is registered.
inline bool noop_image_loader(tinygltf::Image* image,
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

inline size_t component_size(int component_type) {
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

inline size_t num_components(int type) {
    switch (type) {
        case TINYGLTF_TYPE_SCALAR:
            return 1;
        case TINYGLTF_TYPE_VEC2:
            return 2;
        case TINYGLTF_TYPE_VEC3:
            return 3;
        case TINYGLTF_TYPE_VEC4:
            return 4;
        case TINYGLTF_TYPE_MAT4:
            return 16;
        default:
            return 0;
    }
}

/** True when the accessor uses the glTF sparse-storage form.
 *
 * tinygltf parses the sparse descriptor but does NOT apply the overrides to
 * the base buffer, so a caller that ignores this silently reads pre-override
 * data. Every reader below refuses sparse accessors rather than returning
 * quietly wrong geometry. */
inline bool accessor_is_sparse(const tinygltf::Accessor& acc) {
    return acc.sparse.isSparse;
}

/** Resolve an accessor to its backing bytes plus the per-element stride.
 *
 * Returns false (with ``err`` set) for any accessor we cannot read
 * byte-exactly: out-of-range indices, sparse storage, a missing bufferView,
 * an unknown component type, or a view that runs past the buffer. */
inline bool resolve_accessor(const tinygltf::Model& model,
                             int accessor_idx,
                             const unsigned char** out_base,
                             size_t* out_stride,
                             size_t* out_count,
                             size_t* out_comps,
                             int* out_component_type,
                             bool* out_normalized,
                             std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = std::string("accessor ") +
                        std::to_string(accessor_idx) + ": " + why;
        return false;
    };
    if (accessor_idx < 0 ||
        static_cast<size_t>(accessor_idx) >= model.accessors.size()) {
        return fail("index out of range");
    }
    const auto& acc = model.accessors[accessor_idx];
    if (accessor_is_sparse(acc)) {
        return fail(
            "sparse accessors are not supported (tinygltf does not apply the "
            "overrides; re-export the asset without sparse storage)");
    }
    const size_t comps = num_components(acc.type);
    const size_t cs = component_size(acc.componentType);
    if (comps == 0) return fail("unsupported accessor type");
    if (cs == 0) return fail("unsupported component type");
    if (acc.bufferView < 0 ||
        static_cast<size_t>(acc.bufferView) >= model.bufferViews.size()) {
        return fail("missing or out-of-range bufferView");
    }
    const auto& view = model.bufferViews[acc.bufferView];
    if (view.buffer < 0 ||
        static_cast<size_t>(view.buffer) >= model.buffers.size()) {
        return fail("bufferView points at a missing buffer");
    }
    const auto& buf = model.buffers[view.buffer];
    size_t stride = view.byteStride;
    if (stride == 0) stride = cs * comps;
    const size_t base = static_cast<size_t>(acc.byteOffset + view.byteOffset);
    if (acc.count > 0) {
        // Last element only needs its own bytes, not a full stride.
        const size_t span = (acc.count - 1) * stride + cs * comps;
        if (base + span > buf.data.size()) return fail("reads past the buffer");
    }
    *out_base = buf.data.data() + base;
    *out_stride = stride;
    *out_count = acc.count;
    *out_comps = comps;
    *out_component_type = acc.componentType;
    *out_normalized = acc.normalized;
    return true;
}

/** Decode one component to float, honouring the accessor's ``normalized``
 *  flag exactly as the glTF spec defines it. */
inline bool decode_component_float(const unsigned char* p,
                                   int component_type,
                                   bool normalized,
                                   float* out) {
    switch (component_type) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            float f;
            std::memcpy(&f, p, sizeof(f));
            *out = f;
            return true;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
            const std::uint8_t u = *p;
            *out = normalized ? static_cast<float>(u) / 255.0f
                              : static_cast<float>(u);
            return true;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            std::uint16_t u;
            std::memcpy(&u, p, sizeof(u));
            *out = normalized ? static_cast<float>(u) / 65535.0f
                              : static_cast<float>(u);
            return true;
        }
        case TINYGLTF_COMPONENT_TYPE_BYTE: {
            std::int8_t s;
            std::memcpy(&s, p, sizeof(s));
            *out = normalized ? std::max(static_cast<float>(s) / 127.0f, -1.0f)
                              : static_cast<float>(s);
            return true;
        }
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
            std::int16_t s;
            std::memcpy(&s, p, sizeof(s));
            *out = normalized
                       ? std::max(static_cast<float>(s) / 32767.0f, -1.0f)
                       : static_cast<float>(s);
            return true;
        }
        default:
            return false;
    }
}

/** Read any accessor into a flat float array of ``count * components``. */
inline bool read_accessor_floats(const tinygltf::Model& model,
                                 int accessor_idx,
                                 std::vector<float>& out,
                                 size_t* out_comps,
                                 std::string* err) {
    const unsigned char* base = nullptr;
    size_t stride = 0, count = 0, comps = 0;
    int ctype = 0;
    bool normalized = false;
    if (!resolve_accessor(model, accessor_idx, &base, &stride, &count, &comps,
                          &ctype, &normalized, err)) {
        return false;
    }
    const size_t cs = component_size(ctype);
    out.assign(count * comps, 0.0f);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* p = base + i * stride;
        for (size_t k = 0; k < comps; ++k) {
            if (!decode_component_float(p + k * cs, ctype, normalized,
                                        &out[i * comps + k])) {
                if (err) *err = "unsupported component type in float accessor";
                return false;
            }
        }
    }
    if (out_comps) *out_comps = comps;
    return true;
}

/** Read an integer accessor (joint indices, indices) into uint32. */
inline bool read_accessor_uints(const tinygltf::Model& model,
                                int accessor_idx,
                                std::vector<std::uint32_t>& out,
                                size_t* out_comps,
                                std::string* err) {
    const unsigned char* base = nullptr;
    size_t stride = 0, count = 0, comps = 0;
    int ctype = 0;
    bool normalized = false;
    if (!resolve_accessor(model, accessor_idx, &base, &stride, &count, &comps,
                          &ctype, &normalized, err)) {
        return false;
    }
    const size_t cs = component_size(ctype);
    out.assign(count * comps, 0u);
    for (size_t i = 0; i < count; ++i) {
        const unsigned char* p = base + i * stride;
        for (size_t k = 0; k < comps; ++k) {
            const unsigned char* q = p + k * cs;
            std::uint32_t v = 0;
            switch (ctype) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    v = *q;
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    std::uint16_t u;
                    std::memcpy(&u, q, sizeof(u));
                    v = u;
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    std::memcpy(&v, q, sizeof(v));
                    break;
                default:
                    if (err) *err = "unsupported component type in uint accessor";
                    return false;
            }
            out[i * comps + k] = v;
        }
    }
    if (out_comps) *out_comps = comps;
    return true;
}

/** Read a MAT4 float accessor (inverseBindMatrices). glTF stores matrices
 *  column-major, which is glm's layout, so this is a straight copy. */
inline bool read_accessor_mat4(const tinygltf::Model& model,
                               int accessor_idx,
                               std::vector<glm::mat4>& out,
                               std::string* err) {
    std::vector<float> raw;
    size_t comps = 0;
    if (!read_accessor_floats(model, accessor_idx, raw, &comps, err)) {
        return false;
    }
    if (comps != 16) {
        if (err) *err = "inverseBindMatrices accessor is not MAT4";
        return false;
    }
    const size_t n = raw.size() / 16;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        std::memcpy(&out[i][0][0], raw.data() + i * 16, sizeof(float) * 16);
    }
    return true;
}

/** A node's local transform: explicit ``matrix`` if present, else T * R * S. */
inline glm::mat4 node_local_matrix(const tinygltf::Node& n) {
    if (n.matrix.size() == 16) {
        glm::mat4 m(1.0f);
        // glTF stores 16 numbers in column-major order; glm indexes [col][row].
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                m[c][r] = static_cast<float>(n.matrix[c * 4 + r]);
            }
        }
        return m;
    }
    glm::mat4 t(1.0f), r(1.0f), s(1.0f);
    if (n.translation.size() == 3) {
        t = glm::translate(glm::mat4(1.0f),
                           glm::vec3(static_cast<float>(n.translation[0]),
                                     static_cast<float>(n.translation[1]),
                                     static_cast<float>(n.translation[2])));
    }
    if (n.rotation.size() == 4) {
        // glTF quaternions are [x, y, z, w]; glm::quat takes (w, x, y, z).
        r = glm::mat4_cast(glm::quat(static_cast<float>(n.rotation[3]),
                                     static_cast<float>(n.rotation[0]),
                                     static_cast<float>(n.rotation[1]),
                                     static_cast<float>(n.rotation[2])));
    }
    if (n.scale.size() == 3) {
        s = glm::scale(glm::mat4(1.0f),
                       glm::vec3(static_cast<float>(n.scale[0]),
                                 static_cast<float>(n.scale[1]),
                                 static_cast<float>(n.scale[2])));
    }
    return t * r * s;
}

/** node index -> parent node index (-1 for roots). */
inline std::vector<int> build_node_parents(const tinygltf::Model& model) {
    std::vector<int> parents(model.nodes.size(), -1);
    for (size_t i = 0; i < model.nodes.size(); ++i) {
        for (int child : model.nodes[i].children) {
            if (child >= 0 && static_cast<size_t>(child) < parents.size()) {
                parents[static_cast<size_t>(child)] = static_cast<int>(i);
            }
        }
    }
    return parents;
}

/** World matrix of every node, following the parent chain.
 *
 * Works for nodes that are not reachable from any scene too (they simply
 * resolve against whatever ancestors they do have), which keeps malformed
 * exports usable. */
inline std::vector<glm::mat4> build_node_world_matrices(
    const tinygltf::Model& model, const std::vector<int>& parents) {
    const size_t n = model.nodes.size();
    std::vector<glm::mat4> world(n, glm::mat4(1.0f));
    std::vector<char> done(n, 0);
    std::vector<int> chain;
    for (size_t i = 0; i < n; ++i) {
        if (done[i]) continue;
        chain.clear();
        int cur = static_cast<int>(i);
        // Walk to the nearest resolved ancestor, guarding against cycles.
        size_t guard = 0;
        while (cur >= 0 && !done[static_cast<size_t>(cur)] && guard++ <= n) {
            chain.push_back(cur);
            cur = parents[static_cast<size_t>(cur)];
        }
        glm::mat4 acc = (cur >= 0) ? world[static_cast<size_t>(cur)]
                                   : glm::mat4(1.0f);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            acc = acc * node_local_matrix(model.nodes[static_cast<size_t>(*it)]);
            world[static_cast<size_t>(*it)] = acc;
            done[static_cast<size_t>(*it)] = 1;
        }
    }
    return world;
}

/** Longest AABB axis of a POSITION accessor, read from the accessor's
 *  required ``min``/``max`` fields so no BIN decoding is needed. */
inline bool accessor_position_bounds(const tinygltf::Model& model,
                                     int accessor_idx,
                                     glm::vec3* out_min,
                                     glm::vec3* out_max) {
    if (accessor_idx < 0 ||
        static_cast<size_t>(accessor_idx) >= model.accessors.size()) {
        return false;
    }
    const auto& acc = model.accessors[accessor_idx];
    if (acc.minValues.size() < 3 || acc.maxValues.size() < 3) return false;
    *out_min = glm::vec3(static_cast<float>(acc.minValues[0]),
                         static_cast<float>(acc.minValues[1]),
                         static_cast<float>(acc.minValues[2]));
    *out_max = glm::vec3(static_cast<float>(acc.maxValues[0]),
                         static_cast<float>(acc.maxValues[1]),
                         static_cast<float>(acc.maxValues[2]));
    return true;
}

}  // namespace ptv::gltf_accessors
