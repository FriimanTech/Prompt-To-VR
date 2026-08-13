#include "WorldGen.h"

#include <cmath>
#include <map>
#include <utility>

namespace ptv::world {

namespace {

// -- Registry ---------------------------------------------------------------

std::map<std::string, WorldBuilderFactory>& registry() {
    static std::map<std::string, WorldBuilderFactory> r;
    return r;
}

const char* primitive_kind_name(PrimitiveKind k) {
    switch (k) {
        case PrimitiveKind::Box: return "box";
        case PrimitiveKind::Plane: return "plane";
        case PrimitiveKind::Cylinder: return "cylinder";
        case PrimitiveKind::Sphere: return "sphere";
        case PrimitiveKind::Ramp: return "ramp";
    }
    return "box";
}

const char* collision_name(Collision c) {
    switch (c) {
        case Collision::None: return "none";
        case Collision::Box: return "box";
        case Collision::Sphere: return "sphere";
        case Collision::Mesh: return "mesh";
    }
    return "box";
}

nlohmann::json color_json(const Color& c) {
    return nlohmann::json::array({c.r, c.g, c.b});
}

nlohmann::json vec3_json(const Vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

// Serialize a transform with a forced uniform scale of 1.0 (non-uniform scale
// is baked into geometry by the context so the runtime's uniform-scale
// Transform stays valid).
nlohmann::json transform_json(const Transform& t) {
    return nlohmann::json{
        {"position", vec3_json(t.position)},
        {"rotation",
         nlohmann::json::array({t.rotation.x, t.rotation.y, t.rotation.z,
                                t.rotation.w})},
        {"scale", 1.0f},
    };
}

nlohmann::json physics_json(Collision collision) {
    return nlohmann::json{
        {"kind", "static"},
        {"mass_kg", 0.0f},
        {"collision_shape", collision_name(collision)},
        {"restitution", 0.05f},
        {"friction", 0.85f},
    };
}

// -- Concrete WorldContext --------------------------------------------------

class SceneBuilderContext final : public WorldContext {
public:
    SceneBuilderContext(std::uint64_t seed, nlohmann::json asset_map)
        : seed_(seed), asset_map_(std::move(asset_map)) {
        doc_["version"] = 2;
        doc_["generator"] = "ptv_worldgen";
        doc_["entities"] = nlohmann::json::array();
        doc_["lights"] = nlohmann::json::array();
        // Sensible defaults so an empty builder still boots. sky() also
        // populates the ambient/fog/exposure environment knobs (and mirrors
        // ambient_intensity to the top level), so it owns those fields.
        sky(SkyDesc{});
        player_spawn(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f});
    }

    void sky(const SkyDesc& d) override {
        doc_["sky"] = nlohmann::json{
            {"top_color", color_json(d.top)},
            {"horizon_color", color_json(d.horizon)},
            {"sun_direction", vec3_json(d.sun_direction)},
            {"sun_color", color_json(d.sun_color)},
            {"sun_intensity", d.sun_intensity},
            {"ambient_color", color_json(d.ambient_color)},
            {"ambient_intensity", d.ambient_intensity},
            {"fog_density", d.fog_density},
            {"exposure", d.exposure},
        };
        // Keep the flat clear-color field populated for backwards compat.
        doc_["sky_color"] = color_json(d.horizon);
        // Mirror ambient intensity at the top level: the runtime SceneLoader
        // and the scene-compile merge both read it there.
        doc_["ambient_intensity"] = d.ambient_intensity;
    }

    void ground(const GroundDesc& d) override {
        PrimitiveDesc p{};
        p.kind = PrimitiveKind::Box;
        p.size = Vec3{d.size_m, 0.2f, d.size_m};
        p.transform.position = Vec3{0.0f, d.y - 0.1f, 0.0f};
        p.color = d.color;
        p.collision = d.collision;
        p.tags = {"ground", "teleport_target"};
        p.name = "ground";
        spawn_primitive(p);
        ground_y_ = d.y;
        has_ground_ = true;
    }

    void light(const LightDesc& d) override {
        Transform t{};
        t.position = d.position;
        nlohmann::json l{
            {"id", "light_" + std::to_string(light_count_++)},
            {"kind", d.kind},
            {"transform", transform_json(t)},
            {"color_rgb", color_json(d.color)},
            {"intensity", d.intensity},
            {"direction", vec3_json(d.direction)},
            {"range", d.range},
        };
        doc_["lights"].push_back(std::move(l));
    }

    void player_spawn(const Vec3& position, const Vec3& facing) override {
        // Convert a facing vector to a yaw quaternion (about +Y).
        const float yaw = std::atan2(facing.x, -facing.z);
        const float hy = yaw * 0.5f;
        doc_["player"] = nlohmann::json{
            {"spawn",
             nlohmann::json{
                 {"position", vec3_json(position)},
                 {"rotation", nlohmann::json::array({0.0f, std::sin(hy), 0.0f,
                                                     std::cos(hy)})},
             }},
        };
        spawn_y_ = position.y;
    }

    void spawn_primitive(const PrimitiveDesc& d) override {
        // Bake non-uniform transform scale into the primitive extents.
        Vec3 size{d.size.x * d.transform.scale.x, d.size.y * d.transform.scale.y,
                  d.size.z * d.transform.scale.z};
        nlohmann::json e = base_entity(d.name.empty() ? "prim" : d.name, d.tags);
        e["transform"] = transform_json(d.transform);
        e["primitive"] = nlohmann::json{
            {"kind", primitive_kind_name(d.kind)},
            {"size", vec3_json(size)},
            {"color_rgb", color_json(d.color)},
        };
        if (d.collision != Collision::None) {
            e["physics"] = physics_json(d.collision);
        }
        doc_["entities"].push_back(std::move(e));
    }

    void add_mesh(const MeshDesc& d) override {
        // Bake non-uniform scale into vertex positions.
        nlohmann::json positions = nlohmann::json::array();
        const std::size_t vcount = d.positions.size() / 3;
        positions.get_ref<nlohmann::json::array_t&>().reserve(d.positions.size());
        for (std::size_t i = 0; i < vcount; ++i) {
            positions.push_back(d.positions[i * 3 + 0] * d.transform.scale.x);
            positions.push_back(d.positions[i * 3 + 1] * d.transform.scale.y);
            positions.push_back(d.positions[i * 3 + 2] * d.transform.scale.z);
        }
        nlohmann::json mesh{
            {"positions", std::move(positions)},
            {"indices", d.indices},
            {"color_rgb", color_json(d.color)},
        };
        if (!d.normals.empty()) mesh["normals"] = d.normals;

        nlohmann::json e = base_entity(d.name.empty() ? "mesh" : d.name, d.tags);
        e["transform"] = transform_json(d.transform);
        e["mesh_inline"] = std::move(mesh);
        if (d.collision != Collision::None) {
            e["physics"] = physics_json(d.collision);
        }
        doc_["entities"].push_back(std::move(e));
    }

    void place_object(const ObjectPlacement& d) override {
        std::string glb;
        if (asset_map_.is_object() && asset_map_.contains(d.asset_id)) {
            glb = asset_map_.at(d.asset_id).get<std::string>();
        }
        nlohmann::json e = base_entity(d.name.empty() ? d.asset_id : d.name, d.tags);
        e["transform"] = transform_json(d.transform);
        e["mesh"] = nlohmann::json{
            {"asset_id", d.asset_id},
            {"glb_path", glb},
            {"target_scale_meters", 1.0f},
            {"collision_shape", "mesh"},
        };
        e["physics"] = physics_json(Collision::Mesh);
        doc_["entities"].push_back(std::move(e));
    }

    void nav_region(const NavRegionDesc& d) override {
        if (d.id.empty()) return;
        nav_regions_.push_back(nlohmann::json{
            {"id", d.id},
            {"min", vec3_json(d.min)},
            {"max", vec3_json(d.max)},
            {"floor_y", d.floor_y},
        });
    }

    void nav_link(const NavLinkDesc& d) override {
        if (d.from_id.empty() || d.to_id.empty()) return;
        nav_links_.push_back(nlohmann::json{
            {"from", d.from_id},
            {"to", d.to_id},
            {"point", vec3_json(d.point)},
            {"cost_multiplier", d.cost_multiplier},
            {"bidirectional", d.bidirectional},
        });
    }

    std::uint64_t seed() const override { return seed_; }

    nlohmann::json finish() {
        // Guarantee a floor exists so the player never falls through.
        if (!has_ground_) {
            ground(GroundDesc{});
        }
        // Omit the block entirely when nothing was declared: an empty
        // navigation graph and no navigation graph mean different things to
        // the runtime, and only the latter should fall back to steering.
        if (!nav_regions_.empty()) {
            doc_["navigation"] = nlohmann::json{
                {"regions", nav_regions_},
                {"links", nav_links_},
            };
        }
        return doc_;
    }

private:
    nlohmann::json base_entity(const std::string& name,
                               const std::vector<std::string>& tags) {
        nlohmann::json e{
            {"id", "env_" + std::to_string(entity_count_++)},
            {"name", name},
            {"tags", tags},
        };
        return e;
    }

    std::uint64_t seed_;
    nlohmann::json asset_map_;
    nlohmann::json doc_;
    std::vector<nlohmann::json> nav_regions_;
    std::vector<nlohmann::json> nav_links_;
    int entity_count_{0};
    int light_count_{0};
    bool has_ground_{false};
    float ground_y_{0.0f};
    float spawn_y_{0.0f};
};

}  // namespace

// -- Registry API -----------------------------------------------------------

bool register_world_builder(const std::string& id, WorldBuilderFactory factory) {
    registry()[id] = std::move(factory);
    return true;
}

std::vector<std::string> registered_builder_ids(bool include_fallback) {
    std::vector<std::string> ids;
    for (const auto& [id, _] : registry()) {
        if (!include_fallback && id == kFallbackBuilderId) continue;
        ids.push_back(id);
    }
    return ids;
}

std::unique_ptr<IWorldBuilder> make_builder(const std::string& id) {
    auto it = registry().find(id);
    if (it == registry().end()) return nullptr;
    return it->second();
}

nlohmann::json build_scene_json(IWorldBuilder& builder, std::uint64_t seed,
                                const nlohmann::json& asset_map) {
    SceneBuilderContext ctx(seed, asset_map);
    builder.build(ctx);
    nlohmann::json doc = ctx.finish();
    doc["builder_id"] = builder.id();
    return doc;
}

}  // namespace ptv::world
