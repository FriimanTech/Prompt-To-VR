#pragma once

// Material / base-colour image parsing shared by the static and skinned GLB
// loaders. Internal (tinygltf types in the signatures) like GltfAccessors.h,
// which must be included first.

#include <cmath>
#include <string>
#include <vector>

#include "ptv/render/GltfMeshLoad.h"

namespace ptv::gltf_materials {

/** KHR_texture_transform of one material's base-colour texture. Applied to
 *  the UVs at load time, so nothing downstream needs to know it existed. */
struct UvTransform {
    float offset[2]{0.0f, 0.0f};
    float scale[2]{1.0f, 1.0f};
    float rotation{0.0f};
    bool present{false};

    void apply(float& u, float& v) const {
        if (!present) return;
        const float c = std::cos(rotation), s = std::sin(rotation);
        const float su = u * scale[0], sv = v * scale[1];
        const float ru = c * su + s * sv;
        const float rv = -s * su + c * sv;
        u = ru + offset[0];
        v = rv + offset[1];
    }
};

inline double value_number(const tinygltf::Value& v, double fallback) {
    if (v.IsNumber()) return v.GetNumberAsDouble();
    if (v.IsInt()) return static_cast<double>(v.GetNumberAsInt());
    return fallback;
}

/** Parse every glTF material into ``out.materials`` (index-for-index, plus a
 *  trailing default material for primitives without one) and every image a
 *  base-colour texture uses into ``out.images``. ``transforms`` is filled in
 *  step with ``out.materials``. */
inline void parse_materials(const tinygltf::Model& model,
                            bool want_image_bytes,
                            gltf_mesh_load::MeshMaterials& out,
                            std::vector<UvTransform>& transforms) {
    using gltf_mesh_load::AlphaMode;
    using gltf_mesh_load::ImageInfo;
    using gltf_mesh_load::MaterialInfo;

    out.materials.clear();
    out.images.clear();
    transforms.clear();
    std::vector<int> image_slot(model.images.size(), -1);

    auto image_for_texture = [&](int texture_index) -> int {
        if (texture_index < 0 ||
            static_cast<size_t>(texture_index) >= model.textures.size()) {
            return -1;
        }
        const auto& tex = model.textures[static_cast<size_t>(texture_index)];
        const int src = tex.source;
        if (src < 0 || static_cast<size_t>(src) >= model.images.size()) return -1;
        if (image_slot[static_cast<size_t>(src)] >= 0) {
            return image_slot[static_cast<size_t>(src)];
        }
        const auto& img = model.images[static_cast<size_t>(src)];
        ImageInfo info;
        info.name = img.name;
        info.mime = img.mimeType;
        const bool data_uri = img.uri.rfind("data:", 0) == 0;
        if (!img.uri.empty() && !data_uri) info.uri = img.uri;
        auto ext = tex.extensions.find("PTV_texture_astc");
        if (ext != tex.extensions.end() && ext->second.IsObject() &&
            ext->second.Has("uri") && ext->second.Get("uri").IsString()) {
            info.ktx2_uri = ext->second.Get("uri").Get<std::string>();
        }
        if (want_image_bytes) {
            if (!img.image.empty()) {
                info.bytes = img.image;  // captured encoded bytes
            } else if (img.bufferView >= 0 &&
                       static_cast<size_t>(img.bufferView) < model.bufferViews.size()) {
                const auto& bv = model.bufferViews[static_cast<size_t>(img.bufferView)];
                if (bv.buffer >= 0 &&
                    static_cast<size_t>(bv.buffer) < model.buffers.size()) {
                    const auto& data = model.buffers[static_cast<size_t>(bv.buffer)].data;
                    if (bv.byteOffset + bv.byteLength <= data.size()) {
                        info.bytes.assign(
                            data.begin() + static_cast<std::ptrdiff_t>(bv.byteOffset),
                            data.begin() + static_cast<std::ptrdiff_t>(bv.byteOffset +
                                                                     bv.byteLength));
                    }
                }
            }
            if (!info.bytes.empty()) {
                info.hash = gltf_mesh_load::content_hash_hex(info.bytes.data(),
                                                             info.bytes.size());
            }
        }
        out.images.push_back(std::move(info));
        image_slot[static_cast<size_t>(src)] = static_cast<int>(out.images.size()) - 1;
        return image_slot[static_cast<size_t>(src)];
    };

    auto sampler_of = [&](int texture_index, gltf_mesh_load::SamplerInfo& s) {
        if (texture_index < 0 ||
            static_cast<size_t>(texture_index) >= model.textures.size()) {
            return;
        }
        const int si = model.textures[static_cast<size_t>(texture_index)].sampler;
        if (si < 0 || static_cast<size_t>(si) >= model.samplers.size()) return;
        const auto& smp = model.samplers[static_cast<size_t>(si)];
        s.mag_filter = smp.magFilter;
        s.min_filter = smp.minFilter;
        s.wrap_s = smp.wrapS;
        s.wrap_t = smp.wrapT;
    };

    for (const auto& m : model.materials) {
        MaterialInfo mi;
        mi.name = m.name;
        const auto& pbr = m.pbrMetallicRoughness;
        for (size_t k = 0; k < 4 && k < pbr.baseColorFactor.size(); ++k) {
            mi.base_color_factor[k] = static_cast<float>(pbr.baseColorFactor[k]);
        }
        UvTransform tr;
        if (pbr.baseColorTexture.index >= 0) {
            mi.base_color_image = image_for_texture(pbr.baseColorTexture.index);
            mi.base_color_texcoord = pbr.baseColorTexture.texCoord;
            sampler_of(pbr.baseColorTexture.index, mi.sampler);
            auto kt = pbr.baseColorTexture.extensions.find("KHR_texture_transform");
            if (kt != pbr.baseColorTexture.extensions.end() && kt->second.IsObject()) {
                const auto& v = kt->second;
                tr.present = true;
                if (v.Has("offset") && v.Get("offset").IsArray() &&
                    v.Get("offset").ArrayLen() >= 2) {
                    tr.offset[0] = static_cast<float>(value_number(v.Get("offset").Get(0), 0.0));
                    tr.offset[1] = static_cast<float>(value_number(v.Get("offset").Get(1), 0.0));
                }
                if (v.Has("scale") && v.Get("scale").IsArray() &&
                    v.Get("scale").ArrayLen() >= 2) {
                    tr.scale[0] = static_cast<float>(value_number(v.Get("scale").Get(0), 1.0));
                    tr.scale[1] = static_cast<float>(value_number(v.Get("scale").Get(1), 1.0));
                }
                if (v.Has("rotation")) {
                    tr.rotation = static_cast<float>(value_number(v.Get("rotation"), 0.0));
                }
                if (v.Has("texCoord")) {
                    mi.base_color_texcoord =
                        static_cast<int>(value_number(v.Get("texCoord"), mi.base_color_texcoord));
                }
            }
        }
        if (m.alphaMode == "MASK") {
            mi.alpha_mode = AlphaMode::Mask;
        } else if (m.alphaMode == "BLEND") {
            mi.alpha_mode = AlphaMode::Blend;
        }
        mi.alpha_cutoff = static_cast<float>(m.alphaCutoff);
        mi.double_sided = m.doubleSided;
        mi.has_normal_map = m.normalTexture.index >= 0;
        mi.has_metallic_roughness_map = pbr.metallicRoughnessTexture.index >= 0;
        mi.has_occlusion_map = m.occlusionTexture.index >= 0;
        mi.has_emissive_map = m.emissiveTexture.index >= 0;
        out.materials.push_back(std::move(mi));
        transforms.push_back(tr);
    }
    // Default material for primitives that name none (glTF: white, opaque).
    MaterialInfo def;
    def.name = "default";
    out.materials.push_back(def);
    transforms.push_back(UvTransform{});
}

/** Material slot for a primitive's ``material`` index. */
inline int material_slot(const gltf_mesh_load::MeshMaterials& mats, int gltf_index) {
    if (gltf_index >= 0 && static_cast<size_t>(gltf_index) + 1 < mats.materials.size()) {
        return gltf_index;
    }
    return static_cast<int>(mats.materials.size()) - 1;
}

}  // namespace ptv::gltf_materials
