#pragma once

// ---------------------------------------------------------------------------
// ptv::world::IWorldBuilder -- the ONLY engine header that LLM-generated VR
// environment code may include.
//
// Generated world builders live under engine/generated/world/<project_id>/
// and are compiled + run OFFLINE inside the tiny ``ptv_worldgen`` host, which
// links NONE of the engine core (no Vulkan, OpenXR, Jolt, renderer, ECS, or
// platform layer). A builder can therefore never touch or modify the engine;
// it only *describes* a world through the WorldContext facade, and the host
// serializes that description to ``world.scene.json`` -- the same declarative
// contract the runtime already loads.
//
// Within this boundary the author is unrestricted: arbitrary C++ (own structs,
// classes, algorithms, noise, loops, multiple files) and arbitrary geometry
// via WorldContext::add_mesh(...).
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ptv::world {

// --- Plain-old-data math (no engine/GLM dependency in the public surface;
//     generated code may use GLM internally and convert to these). -----------

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct Quat {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};  // identity
};

struct Color {
    float r{1.0f};
    float g{1.0f};
    float b{1.0f};
};

struct Transform {
    Vec3 position{};
    Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};  // non-uniform allowed
};

// How an entity collides. ``None`` = visual only (player passes through).
enum class Collision : std::uint8_t { None, Box, Sphere, Mesh };

enum class PrimitiveKind : std::uint8_t { Box, Plane, Cylinder, Sphere, Ramp };

// --- Descriptors -----------------------------------------------------------

struct SkyDesc {
    Color top{0.05f, 0.07f, 0.15f};
    Color horizon{0.45f, 0.55f, 0.75f};
    Vec3 sun_direction{0.30f, -0.82f, 0.30f};
    Color sun_color{1.0f, 0.97f, 0.9f};
    float sun_intensity{1.0f};
};

struct GroundDesc {
    float size_m{240.0f};
    float y{0.0f};
    Color color{0.20f, 0.21f, 0.24f};
    bool grid{true};
    Collision collision{Collision::Box};
};

struct LightDesc {
    std::string kind{"directional"};  // "directional" | "point"
    Vec3 position{};
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Color color{1.0f, 1.0f, 1.0f};
    float intensity{1.0f};
};

struct PrimitiveDesc {
    PrimitiveKind kind{PrimitiveKind::Box};
    Transform transform{};
    Vec3 size{1.0f, 1.0f, 1.0f};  // full extents in meters
    Color color{0.8f, 0.8f, 0.82f};
    Collision collision{Collision::Box};
    std::vector<std::string> tags{};
    std::string name{};
};

// Arbitrary procedural triangle mesh. ``positions`` and ``normals`` are flat
// xyz triples; ``indices`` is a triangle list. ``normals`` may be empty (the
// host will compute flat normals). This is how an author builds ANY shape.
struct MeshDesc {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint32_t> indices;
    Transform transform{};
    Color color{0.8f, 0.8f, 0.82f};
    Collision collision{Collision::Mesh};
    std::vector<std::string> tags{};
    std::string name{};
};

// Anchor a pipeline-generated prop/character GLB (resolved by asset_id at
// host run time from the asset map). Optional -- the environment itself is
// pure procedural C++; this is only for placing existing 3D objects.
struct ObjectPlacement {
    std::string asset_id;
    Transform transform{};
    std::vector<std::string> tags{};
    std::string name{};
};

// --- The facade the builder talks to (pure data accumulator) ---------------

class WorldContext {
public:
    virtual ~WorldContext() = default;

    // Global environment.
    virtual void sky(const SkyDesc& desc) = 0;
    virtual void ground(const GroundDesc& desc) = 0;
    virtual void light(const LightDesc& desc) = 0;
    virtual void player_spawn(const Vec3& position, const Vec3& facing) = 0;

    // Geometry.
    virtual void spawn_primitive(const PrimitiveDesc& desc) = 0;
    virtual void add_mesh(const MeshDesc& desc) = 0;

    // Optional: place an existing generated object (prop/character) GLB.
    virtual void place_object(const ObjectPlacement& desc) = 0;

    // A deterministic per-world seed (set by the host). Use it for any
    // pseudo-random variation so results are reproducible.
    virtual std::uint64_t seed() const = 0;
};

// --- Builder interface + registry ------------------------------------------

class IWorldBuilder {
public:
    virtual ~IWorldBuilder() = default;

    // Author the entire environment here.
    virtual void build(WorldContext& world) = 0;

    // Stable identifier, e.g. "generated.world.<project_id>".
    virtual std::string id() const = 0;
};

using WorldBuilderFactory = std::function<std::unique_ptr<IWorldBuilder>()>;

// Defined in engine/worldgen. Generated code calls this via the macro below.
bool register_world_builder(const std::string& id, WorldBuilderFactory factory);

#define PTV_DETAIL_WCAT_(a, b) a##b
#define PTV_DETAIL_WCAT(a, b) PTV_DETAIL_WCAT_(a, b)

// Register a world builder at file scope. Safe with a plain or namespace-
// qualified TypeName; the variable name is uniquified by __LINE__/__COUNTER__
// so it never collides or token-pastes ``::`` into an identifier. Must be
// invoked at namespace scope (never inside a class/function body).
#define PTV_REGISTER_WORLD_BUILDER(id_literal, TypeName)                       \
    namespace {                                                                \
    const bool PTV_DETAIL_WCAT(_ptv_regw_,                                     \
                               PTV_DETAIL_WCAT(__LINE__, __COUNTER__)) =       \
        ::ptv::world::register_world_builder(id_literal, [] {                  \
            return std::unique_ptr<::ptv::world::IWorldBuilder>(new TypeName());\
        });                                                                    \
    }

// ---------------------------------------------------------------------------
// Header-only convenience builders. These are OPTIONAL sugar implemented
// purely in terms of the core WorldContext API above, so the host only has to
// implement the virtuals. Authors may ignore them and call spawn_primitive /
// add_mesh directly for full control.
// ---------------------------------------------------------------------------

namespace helpers {

inline Transform at(float x, float y, float z) {
    Transform t{};
    t.position = Vec3{x, y, z};
    return t;
}

// A simple deterministic LCG so scatter/variation is reproducible.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    float unit() {  // [0,1)
        state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((state_ >> 40) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }

private:
    std::uint64_t state_;
};

// A grid of box "buildings" with height variation. Great for cities.
struct BuildingGridDesc {
    Vec3 origin{-60.0f, 0.0f, -60.0f};
    int cols{6};
    int rows{6};
    float spacing{20.0f};
    float footprint{10.0f};
    float min_height{6.0f};
    float max_height{48.0f};
    Color color{0.32f, 0.34f, 0.4f};
    std::uint64_t seed{1};
};

inline void building_grid(WorldContext& w, const BuildingGridDesc& d) {
    Rng rng(d.seed ? d.seed : (w.seed() ^ 0xB5u));
    for (int r = 0; r < d.rows; ++r) {
        for (int c = 0; c < d.cols; ++c) {
            const float h = rng.range(d.min_height, d.max_height);
            PrimitiveDesc p{};
            p.kind = PrimitiveKind::Box;
            p.size = Vec3{d.footprint, h, d.footprint};
            p.transform.position = Vec3{d.origin.x + c * d.spacing,
                                        d.origin.y + h * 0.5f,
                                        d.origin.z + r * d.spacing};
            p.color = d.color;
            p.collision = Collision::Box;
            p.tags = {"building"};
            w.spawn_primitive(p);
        }
    }
}

// Rectangular perimeter walls (+ optional ceiling). Great for rooms/arenas.
struct WallShellDesc {
    Vec3 center{0.0f, 0.0f, 0.0f};
    float width{20.0f};
    float depth{20.0f};
    float height{4.0f};
    float thickness{0.4f};
    Color color{0.4f, 0.4f, 0.44f};
    bool ceiling{false};
};

inline void wall_shell(WorldContext& w, const WallShellDesc& d) {
    const float hw = d.width * 0.5f;
    const float hd = d.depth * 0.5f;
    const float y = d.height * 0.5f;
    auto wall = [&](Vec3 pos, Vec3 size) {
        PrimitiveDesc p{};
        p.kind = PrimitiveKind::Box;
        p.size = size;
        p.transform.position = pos;
        p.color = d.color;
        p.collision = Collision::Box;
        p.tags = {"wall"};
        w.spawn_primitive(p);
    };
    wall({d.center.x, d.center.y + y, d.center.z - hd},
         {d.width, d.height, d.thickness});
    wall({d.center.x, d.center.y + y, d.center.z + hd},
         {d.width, d.height, d.thickness});
    wall({d.center.x - hw, d.center.y + y, d.center.z},
         {d.thickness, d.height, d.depth});
    wall({d.center.x + hw, d.center.y + y, d.center.z},
         {d.thickness, d.height, d.depth});
    if (d.ceiling) {
        wall({d.center.x, d.center.y + d.height, d.center.z},
             {d.width, d.thickness, d.depth});
    }
}

// A flat walkway/road strip on the ground.
struct PathDesc {
    Vec3 start{0.0f, 0.02f, 0.0f};
    Vec3 end{0.0f, 0.02f, 40.0f};
    float width{4.0f};
    Color color{0.15f, 0.15f, 0.17f};
};

inline void path(WorldContext& w, const PathDesc& d) {
    const float dx = d.end.x - d.start.x;
    const float dz = d.end.z - d.start.z;
    const float len = std::max(0.01f, std::sqrt(dx * dx + dz * dz));
    PrimitiveDesc p{};
    p.kind = PrimitiveKind::Box;
    p.size = Vec3{d.width, 0.04f, len};
    p.transform.position =
        Vec3{(d.start.x + d.end.x) * 0.5f, d.start.y, (d.start.z + d.end.z) * 0.5f};
    // yaw so +Z aligns with the path direction
    const float yaw = std::atan2(dx, dz);
    const float hy = yaw * 0.5f;
    p.transform.rotation = Quat{0.0f, std::sin(hy), 0.0f, std::cos(hy)};
    p.color = d.color;
    p.collision = Collision::Box;
    p.tags = {"path"};
    w.spawn_primitive(p);
}

// A set of elevated platforms/steps for verticality.
struct PlatformSetDesc {
    Vec3 origin{0.0f, 0.0f, 0.0f};
    int count{4};
    float step_up{1.0f};
    float step_forward{3.0f};
    Vec3 size{4.0f, 0.4f, 3.0f};
    Color color{0.3f, 0.33f, 0.38f};
};

inline void platform_set(WorldContext& w, const PlatformSetDesc& d) {
    for (int i = 0; i < d.count; ++i) {
        PrimitiveDesc p{};
        p.kind = PrimitiveKind::Box;
        p.size = d.size;
        p.transform.position = Vec3{d.origin.x, d.origin.y + (i + 1) * d.step_up,
                                    d.origin.z + (i + 1) * d.step_forward};
        p.color = d.color;
        p.collision = Collision::Box;
        p.tags = {"platform"};
        w.spawn_primitive(p);
    }
}

}  // namespace helpers

}  // namespace ptv::world
