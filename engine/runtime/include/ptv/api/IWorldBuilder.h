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

/** One value of declared state.
 *
 * A variant rather than a bool because the things worlds actually contain
 * disagree about shape: a traffic light is enumerated, a door is a continuous
 * angle, an elevator is a named floor, and a switch is a flag. Squeezing all
 * of those into a boolean is what forces state into the IDENTIFIER instead,
 * and then an opened ``door_closed_1`` is a name that contradicts reality --
 * while still being the key its saves are written under. */
struct StateValue {
    enum class Type : std::uint8_t { Bool, Number, Name, Vector };
    Type type{Type::Bool};
    bool b{false};
    float number{0.0f};
    std::string name;  // "closed", "green", "floor_3"
    Vec3 vector{};

    static StateValue of(bool v) {
        StateValue s{};
        s.type = Type::Bool;
        s.b = v;
        return s;
    }
    static StateValue of(float v) {
        StateValue s{};
        s.type = Type::Number;
        s.number = v;
        return s;
    }
    static StateValue named(std::string v) {
        StateValue s{};
        s.type = Type::Name;
        s.name = std::move(v);
        return s;
    }
    static StateValue of(const Vec3& v) {
        StateValue s{};
        s.type = Type::Vector;
        s.vector = v;
        return s;
    }
};

// --- Descriptors -----------------------------------------------------------

// Sky gradient PLUS the global environment/atmosphere knobs that shape the
// overall look. You control ALL of these; the engine renders exactly what you
// set here (no hidden brightness floor). See engine/world_author_guide.md for
// the precise rendering contract and how these read on screen.
struct SkyDesc {
    // Visible sky gradient. ``horizon`` also drives the distance-fog color, so
    // distant geometry melts into the sky.
    Color top{0.05f, 0.07f, 0.15f};
    Color horizon{0.45f, 0.55f, 0.75f};

    // The sun: ``sun_direction`` is the direction the light TRAVELS (points
    // away from the sun, i.e. downward-ish). It positions the sun in the sky
    // AND shades every surface, so choose an angle that rakes your key
    // surfaces. Typical ``sun_intensity`` is ~1-4.
    Vec3 sun_direction{0.30f, -0.82f, 0.30f};
    Color sun_color{1.0f, 0.97f, 0.9f};
    float sun_intensity{1.0f};

    // Ambient fill: the light that reaches surfaces the sun / point lights
    // miss (walls facing away, undersides, interiors). ``ambient_color`` is the
    // lower-hemisphere (ground bounce) color; the upper hemisphere blends
    // toward ``top``. ``ambient_intensity`` scales it. Bright, readable scene:
    // ~0.3-0.6. Dark / moody: lower it, but then add point lights so focal
    // surfaces still read. This is YOUR call.
    Color ambient_color{0.20f, 0.21f, 0.24f};
    float ambient_intensity{0.30f};

    // Atmosphere. ``fog_density`` fades distant geometry into ``horizon``
    // (0 = no fog; ~0.01 subtle; higher = thicker). ``exposure`` is a global
    // brightness multiplier applied before tone mapping (1.0 = neutral; >1
    // brighter, <1 darker).
    float fog_density{0.012f};
    float exposure{1.0f};
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
    // Point-light falloff radius in meters (light fades smoothly to zero at
    // ``range``). YOU control this directly; place lights near the surfaces
    // you want lit and size the range to reach them. Ignored for
    // "directional" lights.
    float range{20.0f};
};

struct PrimitiveDesc {
    PrimitiveKind kind{PrimitiveKind::Box};
    Transform transform{};
    Vec3 size{1.0f, 1.0f, 1.0f};  // full extents in meters
    Color color{0.8f, 0.8f, 0.82f};
    Collision collision{Collision::Box};
    /** Gameplay tags. Several are already read by the engine and cost
     *  nothing to add: ``grabbable`` makes an object pickable by
     *  ``builtin.grab`` with no further wiring, ``ground`` /
     *  ``teleport_target`` mark walkable floor, ``mirror_visible`` puts the
     *  object in a mirror. */
    std::vector<std::string> tags{};
    /** Becomes the entity's stable id, so give everything a meaningful name.
     *  Repeats are disambiguated deterministically (wall, wall_2, wall_3). */
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
/** A rectangle of walkable floor, declared as you build it.
 *
 * NPCs need a model of where they can walk and how those places connect.
 * The usual way to get one is to voxelise the finished level (Recast), but
 * that reconstructs something you already know: you are the one placing this
 * room and that doorway. Declaring it directly is cheaper, exact, and cannot
 * disagree with your intent.
 *
 * Cover the floor the player and NPCs are meant to use. Overlap at doorways
 * is fine and helpful. A region is a flat rectangle, so a sloped or stepped
 * area becomes several regions linked together.
 *
 * Declare nothing and NPCs fall back to local steering: fine in an open
 * arena, useless for getting through a door. */
struct NavRegionDesc {
    std::string id;
    /** Opposite corners of the footprint, in metres. Y is ignored. */
    Vec3 min{};
    Vec3 max{};
    /** Height of the floor in this region. */
    float floor_y{0.0f};
};

/** A way through between two regions: a doorway, a gap, the top of a ramp. */
struct NavLinkDesc {
    std::string from_id;
    std::string to_id;
    /** Where to cross. Put it IN the opening -- this is the point NPCs aim
     *  at, and it is what makes them walk through a door instead of into the
     *  wall beside it. */
    Vec3 point{};
    /** Above 1.0 makes the route less attractive, so a long way round can be
     *  preferred over a ladder. */
    float cost_multiplier{1.0f};
    bool bidirectional{true};
    /** Interactive whose state controls this crossing, or empty for a link
     *  that is always open.
     *
     *  Without this a nav link is an UNCONDITIONAL assertion that the route
     *  exists, so an NPC walks through a closed door -- the link knows a
     *  doorway is there but nothing tells it a door is in the doorway. Name
     *  the door here and give the state that permits passage. */
    std::string gated_by{};
    /** State key and value on ``gated_by`` that allow passage, e.g.
     *  ("open", true) or ("state", "raised"). */
    std::string gate_state_key{};
    StateValue gate_state_value{};
};

/** Something in the world whose state can change, or that gameplay can act
 *  on.
 *
 * Declaring this is what lets the rest of the engine stop guessing. Without
 * it nothing knows that the thing in the doorway IS a door, whether it starts
 * open, or what "open" would even mean for it -- so navigation asserts an
 * unconditional crossing, and persistence has no pristine value to compare a
 * change against.
 *
 * Identity and state are deliberately SEPARATE. ``id`` is permanent and is
 * the key saves are written under; ``initial_state`` is what changes. Encode
 * the state in the name instead and the key starts lying the moment the
 * player touches it.
 *
 * Declare this only for things whose configuration can change or that
 * gameplay can touch. Static scenery stays plain geometry: every interactive
 * costs runtime bookkeeping, and a Quest 3 busy scene has a budget of roughly
 * 200-300 draw calls to work inside. Never declare thousands of small ones --
 * a grass field is batched geometry plus ONE ``harvestable`` region, because
 * measured VR foliage tests fall under 10 FPS at around 2,000 instances. */
struct InteractiveDesc {
    /** Stable, unique, author-chosen. This is the persistence key for the
     *  life of the project, so it must not encode anything that changes. */
    std::string id;
    /** One of: "door", "switchable", "container", "mover", "breakable",
     *  "harvestable", "movable". */
    std::string kind;
    /** Named state and its value at load, e.g. {"open", of(false)} or
     *  {"floor", named("floor_1")}. */
    std::vector<std::pair<std::string, StateValue>> initial_state{};

    // --- Kind parameters. Only the relevant ones are read. -------------
    /** Hinge for a door or lid: a point and an axis, plus the range it may
     *  swing through. A standard door is 0 to 90; some go past it. */
    Vec3 hinge_point{};
    Vec3 hinge_axis{0.0f, 1.0f, 0.0f};
    float min_angle_deg{0.0f};
    float max_angle_deg{90.0f};
    /** Slide for a drawer or sliding door. */
    Vec3 slide_axis{};
    float slide_distance_m{0.0f};
    /** Named positions for a mover: elevator floors, raised/lowered. */
    std::vector<std::string> positions{};
    /** What one pick yields, for a harvestable region. */
    std::string yields_asset_id{};
    /** A MODIFIER, not a kind: a gate can be closed AND locked, and the two
     *  are independent questions. */
    bool locked{false};
    std::vector<std::string> tags{};
};

struct ObjectPlacement {
    std::string asset_id;
    Transform transform{};
    std::vector<std::string> tags{};
    std::string name{};
};

/** A named place to arrive at.
 *
 * More than one is needed because returning from a mission or an exhibit has
 * to put the player back at the door they left through, not at the world
 * origin. */
struct SpawnPointDesc {
    std::string id;  // "mission_start", "returned_from_hospital"
    Vec3 position{};
    Vec3 facing{0.0f, 0.0f, -1.0f};
};

/** Where NPCs enter the level: a gate the enemies pour through, a cave
 * mouth, a landing pad, a ruined archway.
 *
 * The encounter director spawns each wave's NPCs inside these, away from
 * the player and out of their sight. Declare several, spread around the
 * level and AWAY from the player spawn (12 m or more), on walkable floor
 * inside a declared nav_region. ``tags`` let a game send different NPC
 * types in through different entrances ("enemy_entry", "boss_gate"). */
struct NpcSpawnZoneDesc {
    std::string id;           // "west_gate", "cave_mouth"
    Vec3 position{};          // on the floor
    float radius{2.0f};       // spawns scatter inside this disc
    std::vector<std::string> tags{};
};

/** A way out of this environment and into another.
 *
 * This is what the author places -- the gate, the doorway, the fire truck --
 * and the engine wires to a transition. A mission chosen from a menu needs no
 * portal geometry and uses a scene link instead. */
struct PortalDesc {
    std::string id;
    std::string to_scene;         // target environment id
    std::string target_spawn_id;  // named spawn in the target
    Vec3 position{};
    Vec3 size{1.0f, 2.0f, 0.5f};  // trigger volume, full extents
    /** False makes it transition on entry rather than on a deliberate
     *  interaction. */
    bool requires_interaction{true};
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

    // Navigation. Declare the walkable floor and the openings between the
    // parts of it, so NPCs can route through doorways and around walls
    // rather than only reacting to whatever is directly in front of them.
    virtual void nav_region(const NavRegionDesc& desc) = 0;
    virtual void nav_link(const NavLinkDesc& desc) = 0;

    // Declare something whose state can change or that gameplay can act on.
    // Call this AFTER the geometry it describes, and give it that geometry's
    // name so the two are bound together. See InteractiveDesc.
    virtual void interactive(const InteractiveDesc& desc) = 0;

    // Named arrival points. ``player_spawn`` sets the default one; these are
    // the extra destinations transitions can target by name.
    virtual void spawn_point(const SpawnPointDesc& desc) = 0;

    // A way out of this environment into another.
    virtual void portal(const PortalDesc& desc) = 0;

    // Where NPC waves enter the level. Declare these whenever the game has
    // enemies or other NPCs that arrive during play. See NpcSpawnZoneDesc.
    virtual void npc_spawn_zone(const NpcSpawnZoneDesc& desc) { (void)desc; }

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
            // Named per grid cell, not left blank. Entity ids are derived
            // from these names, and an unnamed primitive falls back to a
            // positional suffix -- which is exactly the instability that
            // makes saved state point at the wrong object after a rebuild.
            p.name = "building_r" + std::to_string(r) + "_c" +
                     std::to_string(c);
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
    // Named by which side they are, so the ids stay meaningful and stable.
    auto wall = [&](const char* side, Vec3 pos, Vec3 size) {
        PrimitiveDesc p{};
        p.kind = PrimitiveKind::Box;
        p.size = size;
        p.transform.position = pos;
        p.color = d.color;
        p.collision = Collision::Box;
        p.tags = {"wall"};
        p.name = std::string("wall_") + side;
        w.spawn_primitive(p);
    };
    wall("north", {d.center.x, d.center.y + y, d.center.z - hd},
         {d.width, d.height, d.thickness});
    wall("south", {d.center.x, d.center.y + y, d.center.z + hd},
         {d.width, d.height, d.thickness});
    wall("west", {d.center.x - hw, d.center.y + y, d.center.z},
         {d.thickness, d.height, d.depth});
    wall("east", {d.center.x + hw, d.center.y + y, d.center.z},
         {d.thickness, d.height, d.depth});
    if (d.ceiling) {
        wall("ceiling", {d.center.x, d.center.y + d.height, d.center.z},
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
    p.name = "path";
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
        p.name = "platform_" + std::to_string(i + 1);
        w.spawn_primitive(p);
    }
}

}  // namespace helpers

}  // namespace ptv::world
