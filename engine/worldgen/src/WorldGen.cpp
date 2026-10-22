#include "WorldGen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
        // An asset map entry is either a bare GLB path (older pipelines) or
        // {"glb_path", "target_scale_meters"}. The scale MUST come from the
        // asset manifest: hard-coding 1.0 here made every world-placed
        // pistol a metre long while the gameplay copy of the same pistol was
        // 24 cm, and which size a player saw depended on which copy's mesh
        // streamed in first.
        std::string glb;
        float scale_m = 1.0f;
        if (asset_map_.is_object() && asset_map_.contains(d.asset_id)) {
            const auto& entry = asset_map_.at(d.asset_id);
            if (entry.is_string()) {
                glb = entry.get<std::string>();
            } else if (entry.is_object()) {
                glb = entry.value("glb_path", std::string{});
                scale_m = entry.value("target_scale_meters", 1.0f);
            }
        }
        nlohmann::json e = base_entity(d.name.empty() ? d.asset_id : d.name, d.tags);
        e["transform"] = transform_json(d.transform);
        e["mesh"] = nlohmann::json{
            {"asset_id", d.asset_id},
            {"glb_path", glb},
            {"target_scale_meters", scale_m},
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
        nlohmann::json l{
            {"from", d.from_id},
            {"to", d.to_id},
            {"point", vec3_json(d.point)},
            {"cost_multiplier", d.cost_multiplier},
            {"bidirectional", d.bidirectional},
        };
        // A gate turns an unconditional assertion that the route exists into
        // one conditional on an interactive's state, which is what stops an
        // NPC walking through a door the player just shut.
        if (!d.gated_by.empty()) {
            l["gated_by"] = d.gated_by;
            l["gate_state_key"] =
                d.gate_state_key.empty() ? "open" : d.gate_state_key;
            l["gate_state_value"] = state_value_json(d.gate_state_value);
        }
        nav_links_.push_back(std::move(l));
    }

    void interactive(const InteractiveDesc& d) override {
        nlohmann::json state = nlohmann::json::object();
        for (const auto& [key, value] : d.initial_state) {
            state[key] = state_value_json(value);
        }
        nlohmann::json i{
            {"id", d.id},
            {"kind", d.kind},
            {"initial_state", std::move(state)},
            {"locked", d.locked},
            {"tags", d.tags},
        };
        // Kind parameters are emitted only when they carry information, so a
        // switch's record does not claim to have a hinge.
        if (d.kind == "door" || d.kind == "container" || d.kind == "mover") {
            i["hinge_point"] = vec3_json(d.hinge_point);
            i["hinge_axis"] = vec3_json(d.hinge_axis);
            i["min_angle_deg"] = d.min_angle_deg;
            i["max_angle_deg"] = d.max_angle_deg;
            if (d.slide_distance_m > 0.0f) {
                i["slide_axis"] = vec3_json(d.slide_axis);
                i["slide_distance_m"] = d.slide_distance_m;
            }
        }
        if (!d.positions.empty()) i["positions"] = d.positions;
        if (!d.yields_asset_id.empty()) {
            i["yields_asset_id"] = d.yields_asset_id;
        }
        interactives_.push_back(std::move(i));
    }

    void npc_spawn_zone(const NpcSpawnZoneDesc& d) override {
        nlohmann::json tags = nlohmann::json::array();
        for (const auto& t : d.tags) tags.push_back(t);
        npc_spawn_zones_.push_back(nlohmann::json{
            {"id", d.id},
            {"position", vec3_json(d.position)},
            {"radius", std::max(0.0f, d.radius)},
            {"tags", tags},
        });
    }

    void spawn_point(const SpawnPointDesc& d) override {
        const float yaw = std::atan2(d.facing.x, -d.facing.z);
        const float hy = yaw * 0.5f;
        spawn_points_.push_back(nlohmann::json{
            {"id", d.id},
            {"position", vec3_json(d.position)},
            {"rotation", nlohmann::json::array({0.0f, std::sin(hy), 0.0f,
                                                std::cos(hy)})},
        });
    }

    void portal(const PortalDesc& d) override {
        portals_.push_back(nlohmann::json{
            {"id", d.id},
            {"to_scene", d.to_scene},
            {"target_spawn_id", d.target_spawn_id},
            {"position", vec3_json(d.position)},
            {"size", vec3_json(d.size)},
            {"requires_interaction", d.requires_interaction},
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
        // Same reasoning for the rest: absent means "this world declared
        // none", which a validator has to be able to tell apart from empty.
        if (!interactives_.empty()) doc_["interactives"] = interactives_;
        if (!spawn_points_.empty()) doc_["spawn_points"] = spawn_points_;
        if (!npc_spawn_zones_.empty()) doc_["npc_spawn_zones"] = npc_spawn_zones_;
        if (!portals_.empty()) doc_["portals"] = portals_;
        return doc_;
    }

private:
    /** Serialize a StateValue, tagging the variant so a reader can tell an
     *  enumerated "3" from the number 3. */
    static nlohmann::json state_value_json(const StateValue& v) {
        switch (v.type) {
            case StateValue::Type::Bool:
                return nlohmann::json{{"type", "bool"}, {"value", v.b}};
            case StateValue::Type::Number:
                return nlohmann::json{{"type", "number"}, {"value", v.number}};
            case StateValue::Type::Name:
                return nlohmann::json{{"type", "name"}, {"value", v.name}};
            case StateValue::Type::Vector:
            default:
                return nlohmann::json{{"type", "vector"},
                                      {"value", vec3_json(v.vector)}};
        }
    }

    /** Slugify a name into an id fragment: lowercase, alphanumeric and
     *  underscores only, runs collapsed. */
    static std::string slugify(const std::string& name) {
        std::string out;
        out.reserve(name.size());
        bool last_us = false;
        for (const char c : name) {
            const auto u = static_cast<unsigned char>(c);
            // ASCII alphanumerics ONLY. A name written with accents or in a
            // non-Latin script arrives here as multi-byte UTF-8, and
            // std::isalnum on those bytes is locale-dependent -- so passing
            // them through would put raw bytes in an id that is both a JSON
            // key and a persistence key. Treat them as separators instead:
            // the id becomes less pretty, never invalid.
            if (u < 128 && std::isalnum(u) != 0) {
                out += static_cast<char>(std::tolower(u));
                last_us = false;
            } else if (!out.empty() && !last_us) {
                out += '_';
                last_us = true;
            }
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return out;
    }

    nlohmann::json base_entity(const std::string& name,
                               const std::vector<std::string>& tags) {
        // Ids are derived from the author's own name, NOT from a positional
        // counter. The old "env_<n>" scheme meant inserting one
        // spawn_primitive call renumbered every entity after it -- and since
        // these ids are the keys saved state is written under, a regenerated
        // world would silently restore the wrong objects. Repeats get a
        // deterministic numeric suffix so ids stay unique without depending
        // on call order for the FIRST occurrence.
        std::string base = slugify(name);
        if (base.empty()) base = "entity";
        const int seen = ++name_counts_[base];
        std::string id = seen == 1 ? base : base + "_" + std::to_string(seen);
        ++entity_count_;

        nlohmann::json e{
            {"id", id},
            {"name", name},
            {"tags", tags},
        };
        entity_ids_.push_back(id);
        return e;
    }

    std::uint64_t seed_;
    nlohmann::json asset_map_;
    nlohmann::json doc_;
    std::vector<nlohmann::json> nav_regions_;
    std::vector<nlohmann::json> nav_links_;
    std::vector<nlohmann::json> interactives_;
    std::vector<nlohmann::json> spawn_points_;
    std::vector<nlohmann::json> npc_spawn_zones_;
    std::vector<nlohmann::json> portals_;
    /** Occurrences per slugified name, so a repeated name gets a stable
     *  suffix instead of every entity depending on call order. */
    std::unordered_map<std::string, int> name_counts_;
    std::vector<std::string> entity_ids_;
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
