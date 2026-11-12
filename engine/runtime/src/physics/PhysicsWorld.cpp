#include "ptv/physics/PhysicsWorld.h"

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"
#include "ptv/render/GltfMeshLoad.h"

// Jolt uses many macros that conflict with common names; keep the include set
// narrow and local.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Physics/Body/AllowedDOFs.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ptv {

namespace {

// Object layers: what a body IS decides what it may touch (see
// ``layers_collide`` and docs/physics.md). Two broad-phase layers: the
// static world, and everything that moves.
namespace Layers {
    static constexpr JPH::ObjectLayer STATIC = 0;       // level geometry
    static constexpr JPH::ObjectLayer MOVING = 1;       // NPC capsules, kinematic world
    static constexpr JPH::ObjectLayer PROP = 2;         // dynamic props
    static constexpr JPH::ObjectLayer HELD = 3;         // held / worn / attached items
    static constexpr JPH::ObjectLayer PLAYER_BODY = 4;  // the player's contact capsule
    static constexpr JPH::ObjectLayer PLAYER_PART = 5;  // the player's hands, forearms, shins
    static constexpr JPH::ObjectLayer DEBRIS = 6;       // shards, casings
    static constexpr JPH::ObjectLayer NUM = 7;
}  // namespace Layers
namespace BPLayers {
    static constexpr JPH::BroadPhaseLayer STATIC{0};
    static constexpr JPH::BroadPhaseLayer MOVING{1};
    static constexpr uint32_t NUM = 2;
}  // namespace BPLayers

/** The pair table. Symmetric; ``true`` means the two may touch.
 *
 *  The player's BODY touches the world and NPCs, never a prop, a held
 *  item or its own parts: an invisible capsule shoving a pistol off a table
 *  as the player walks up to it is the bug this exists to prevent. The
 *  player's PARTS (hands, forearms, shins) do touch props, which is what a
 *  real hand nudging a can looks like. Held items never touch their holder's
 *  body or parts. */
bool layers_collide(JPH::ObjectLayer a, JPH::ObjectLayer b) {
    if (a > b) std::swap(a, b);
    using namespace Layers;
    switch (a) {
        case STATIC:
            return b != STATIC && b != PLAYER_PART;
        case MOVING:
            return b == MOVING || b == PROP || b == HELD || b == PLAYER_BODY;
        case PROP:
            return b == PROP || b == HELD || b == PLAYER_PART || b == DEBRIS;
        case HELD:
            return b == HELD;
        case PLAYER_BODY:
        case PLAYER_PART:
            return a == PLAYER_PART && b == DEBRIS;
        default:
            return false;
    }
}

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterface() {
        for (JPH::ObjectLayer l = 0; l < Layers::NUM; ++l) map_[l] = BPLayers::MOVING;
        map_[Layers::STATIC] = BPLayers::STATIC;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return map_[layer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
        return "BPLayer";
    }
#endif
private:
    JPH::BroadPhaseLayer map_[Layers::NUM]{};
};

class ObjectVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        if (bp == BPLayers::STATIC) return layers_collide(layer, Layers::STATIC);
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return layers_collide(a, b);
    }
};

/** What the player's CharacterVirtual sweeps against: the world, NPCs and
 *  props. Light props are then rejected by its contact listener, heavy ones
 *  block without being pushed. */
class PlayerSweepLayerFilter final : public JPH::ObjectLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l) const override {
        return l == Layers::STATIC || l == Layers::MOVING || l == Layers::PROP;
    }
};

/** Floor queries: level geometry, plus moving bodies explicitly tagged
 *  ``walkable`` (a lift, a raft), checked per body. */
class FloorLayerFilter final : public JPH::ObjectLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l) const override {
        return l == Layers::STATIC || l == Layers::MOVING;
    }
};

/** Props lighter than this never stop the player's body; heavier ones
 *  (a crate, a barrel) block it without being shoved. */
constexpr float kPlayerBlockingPropKg = 25.0f;

/** The player's body never pushes a prop by walking into it.
 *
 *  Jolt's defaults (``mCanReceiveImpulses = true``, ``mCanPushCharacter =
 *  true``) let the swept capsule impart impulses to every dynamic body it
 *  touches, which is what shoved a pistol off its table as the player
 *  reached for it. Light props are not contacts at all; heavy ones are
 *  solid but immovable to the body. Hands and forearms, which ARE allowed
 *  to nudge props, are separate kinematic bodies (``PLAYER_PART``). */
class PlayerContactListener final : public JPH::CharacterContactListener {
public:
    explicit PlayerContactListener(JPH::PhysicsSystem& system) : system_(system) {}

    bool OnContactValidate(const JPH::CharacterVirtual*, const JPH::BodyID& body,
                           const JPH::SubShapeID&) override {
        const JPH::BodyInterface& bi = system_.GetBodyInterfaceNoLock();
        const JPH::ObjectLayer l = bi.GetObjectLayer(body);
        if (l == Layers::HELD || l == Layers::DEBRIS || l == Layers::PLAYER_PART ||
            l == Layers::PLAYER_BODY) {
            return false;
        }
        if (l == Layers::PROP) return mass_of(body) >= kPlayerBlockingPropKg;
        return true;
    }

    void OnContactAdded(const JPH::CharacterVirtual*, const JPH::BodyID& body,
                        const JPH::SubShapeID&, JPH::RVec3Arg, JPH::Vec3Arg,
                        JPH::CharacterContactSettings& settings) override {
        configure(body, settings);
    }

    void OnContactPersisted(const JPH::CharacterVirtual*, const JPH::BodyID& body,
                            const JPH::SubShapeID&, JPH::RVec3Arg, JPH::Vec3Arg,
                            JPH::CharacterContactSettings& settings) override {
        configure(body, settings);
    }

private:
    void configure(const JPH::BodyID& body, JPH::CharacterContactSettings& settings) const {
        if (system_.GetBodyInterfaceNoLock().GetObjectLayer(body) == Layers::PROP) {
            settings.mCanReceiveImpulses = false;
            settings.mCanPushCharacter = false;
        }
    }

    float mass_of(const JPH::BodyID& body) const {
        JPH::BodyLockRead lock(system_.GetBodyLockInterfaceNoLock(), body);
        if (!lock.Succeeded()) return 0.0f;
        const JPH::MotionProperties* mp = lock.GetBody().GetMotionProperties();
        if (mp == nullptr) return 0.0f;
        const float inv = mp->GetInverseMass();
        return inv > 1e-9f ? 1.0f / inv : 0.0f;
    }

    JPH::PhysicsSystem& system_;
};

/** Pairs the simulation must not resolve this frame: a player part and the
 *  prop its hand is reaching to grab. Without this the fingers close in on
 *  a pistol and push it away before the grip can take it. Read on Jolt's
 *  job threads during ``step``; written only between steps. */
class PartContactFilter final : public JPH::ContactListener {
public:
    const std::unordered_map<std::uint32_t, int>* part_group{nullptr};
    const std::vector<std::unordered_set<std::uint32_t>>* part_ignore{nullptr};

    JPH::ValidateResult OnContactValidate(const JPH::Body& a, const JPH::Body& b,
                                          JPH::RVec3Arg, const JPH::CollideShapeResult&) override {
        if (part_group == nullptr || part_ignore == nullptr) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        const JPH::Body* part = nullptr;
        const JPH::Body* other = nullptr;
        if (a.GetObjectLayer() == Layers::PLAYER_PART) {
            part = &a;
            other = &b;
        } else if (b.GetObjectLayer() == Layers::PLAYER_PART) {
            part = &b;
            other = &a;
        } else {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        const auto g = part_group->find(part->GetID().GetIndexAndSequenceNumber());
        if (g == part_group->end() || g->second < 0 ||
            g->second >= static_cast<int>(part_ignore->size())) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }
        const auto& ignore = (*part_ignore)[static_cast<std::size_t>(g->second)];
        if (ignore.count(other->GetID().GetIndexAndSequenceNumber()) != 0) {
            return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
};

/** Layer for a body class from the public header. */
JPH::ObjectLayer layer_for(PhysicsWorld::BodyClass c) {
    switch (c) {
        case PhysicsWorld::BodyClass::Static: return Layers::STATIC;
        case PhysicsWorld::BodyClass::Moving: return Layers::MOVING;
        case PhysicsWorld::BodyClass::Prop: return Layers::PROP;
        case PhysicsWorld::BodyClass::Held: return Layers::HELD;
        case PhysicsWorld::BodyClass::PlayerBody: return Layers::PLAYER_BODY;
        case PhysicsWorld::BodyClass::PlayerPart: return Layers::PLAYER_PART;
        case PhysicsWorld::BodyClass::Debris: return Layers::DEBRIS;
    }
    return Layers::MOVING;
}

PhysicsWorld::BodyClass class_for(JPH::ObjectLayer l) {
    switch (l) {
        case Layers::STATIC: return PhysicsWorld::BodyClass::Static;
        case Layers::PROP: return PhysicsWorld::BodyClass::Prop;
        case Layers::HELD: return PhysicsWorld::BodyClass::Held;
        case Layers::PLAYER_BODY: return PhysicsWorld::BodyClass::PlayerBody;
        case Layers::PLAYER_PART: return PhysicsWorld::BodyClass::PlayerPart;
        case Layers::DEBRIS: return PhysicsWorld::BodyClass::Debris;
        default: return PhysicsWorld::BodyClass::Moving;
    }
}

// Build a static triangle-mesh collision shape from an inline procedural mesh.
// Returns a null ref on any problem (caller falls back to a box).
JPH::ShapeRefC build_inline_mesh_shape(const CInlineMesh& im) {
    const std::size_t vcount = im.positions.size() / 3;
    if (vcount < 3 || im.indices.size() < 3) return {};

    JPH::VertexList verts;
    verts.reserve(static_cast<JPH::uint>(vcount));
    for (std::size_t i = 0; i < vcount; ++i) {
        verts.push_back(JPH::Float3(im.positions[i * 3 + 0],
                                    im.positions[i * 3 + 1],
                                    im.positions[i * 3 + 2]));
    }

    JPH::IndexedTriangleList tris;
    const std::size_t tcount = im.indices.size() / 3;
    tris.reserve(static_cast<JPH::uint>(tcount));
    for (std::size_t i = 0; i < tcount; ++i) {
        const std::uint32_t a = im.indices[i * 3 + 0];
        const std::uint32_t b = im.indices[i * 3 + 1];
        const std::uint32_t c = im.indices[i * 3 + 2];
        if (a >= vcount || b >= vcount || c >= vcount) continue;
        if (a == b || b == c || a == c) continue;  // skip degenerate
        tris.push_back(JPH::IndexedTriangle(a, b, c, 0));
    }
    if (tris.empty()) return {};

    JPH::MeshShapeSettings settings(verts, tris);
    settings.Sanitize();
    JPH::ShapeSettings::ShapeResult res = settings.Create();
    if (res.HasError()) return {};
    return res.Get();
}

/** Upright capsule for a character, sized from the mesh's own metrics.
 *
 * A character's longest axis is its height, and that is exactly what the
 * asset pipeline normalizes to ``target_scale_meters``, so height comes
 * straight from there; ``bounds_radius_m`` (half the longest extent, already
 * in metres) is the fallback for meshes that never went through
 * normalization.
 *
 * The radius is a fraction of height rather than a second measured extent on
 * purpose: a Tripo character in a T-pose has an arm span roughly equal to its
 * height, so a measured X extent would produce a capsule wide enough to wedge
 * the NPC in any doorway. 0.17 puts a 1.8 m humanoid at a 0.31 m radius,
 * which matches the shoulder width that actually has to fit through gaps. */
/** Forward Jolt's own diagnostics into the engine log.
 *
 *  This used to be set to nullptr, which silenced Jolt entirely AND left a
 *  function pointer Jolt is documented to call -- so the first trace would
 *  have jumped to address zero. */
void jolt_trace(const char* fmt, ...) {
    if (fmt == nullptr) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) PTV_WARN("Jolt: {}", buf);
}

#ifdef JPH_ENABLE_ASSERTS
/** Report a failed Jolt precondition instead of halting the process.
 *
 *  JPH_ASSERT only executes JPH_BREAKPOINT when this returns true, so
 *  returning false downgrades every Jolt assert to a logged error. That
 *  distinction is the difference between a generated game that reports a bad
 *  body and one that vanishes: on a headset a breakpoint is SIGTRAP, the
 *  process dies inside android_main before any window exists, and the user
 *  sees the home environment reappear with no error at all.
 *
 *  Continuing is the right call for this engine specifically. Every scene it
 *  loads was authored by a language model, so a precondition violation is an
 *  expected data defect rather than a logic error in code we control, and the
 *  guards at each use site already substitute safe values. */
bool jolt_assert_failed(const char* expression, const char* message,
                        const char* file, JPH::uint line) {
    PTV_ERROR("Jolt assert failed: {} ({}:{}){}{}",
              expression != nullptr ? expression : "?",
              file != nullptr ? file : "?", line,
              message != nullptr ? " -- " : "",
              message != nullptr ? message : "");
    return false;  // do NOT breakpoint
}
#endif

/** Mass to substitute when a movable body was authored without one.
 *
 *  1 kg keeps a dynamic body's response in a plausible range for the
 *  hand-sized props this usually hits, and is unobservable on a kinematic
 *  body, which is driven by its transform rather than by forces. */
constexpr float kFallbackMassKg = 1.0f;

JPH::ShapeRefC build_capsule_shape(float height_m, float radius_m = 0.0f) {
    const float height = std::max(0.20f, height_m);
    const float radius = std::clamp(radius_m > 1e-3f ? radius_m : 0.17f * height, 0.05f,
                                    0.45f * height);
    const float half_cylinder = std::max(0.02f, 0.5f * height - radius);
    return new JPH::CapsuleShape(half_cylinder, radius);
}

bool is_finite_vec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/** Body filter that skips a set of bodies. */
class IgnoreBodiesFilter final : public JPH::BodyFilter {
public:
    std::vector<JPH::BodyID> ids;
    bool ShouldCollide(const JPH::BodyID& id) const override {
        return std::find(ids.begin(), ids.end(), id) == ids.end();
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return !body.IsSensor();
    }
};

/** Capsule with its origin at the FEET, which is what a player body is
 *  positioned by. */
JPH::ShapeRefC build_feet_capsule(float height, float radius) {
    const float h = std::max(2.0f * radius + 0.05f, height);
    const float half_cyl = std::max(0.01f, 0.5f * h - radius);
    JPH::RotatedTranslatedShapeSettings settings(
        JPH::Vec3(0.0f, 0.5f * h, 0.0f), JPH::Quat::sIdentity(),
        new JPH::CapsuleShape(half_cyl, radius));
    JPH::ShapeSettings::ShapeResult res = settings.Create();
    if (res.HasError()) return {};
    return res.Get();
}

/** ``inner`` moved so its origin sits at ``offset`` in body space. */
JPH::ShapeRefC offset_shape(JPH::ShapeRefC inner, const Vec3& offset) {
    if (inner.GetPtr() == nullptr) return inner;
    if (glm::length(offset) < 1.0e-4f) return inner;
    JPH::RotatedTranslatedShapeSettings settings(
        JPH::Vec3(offset.x, offset.y, offset.z), JPH::Quat::sIdentity(), inner);
    JPH::ShapeSettings::ShapeResult res = settings.Create();
    if (res.HasError()) return inner;
    return res.Get();
}

/** A box whose convex radius never exceeds its thinnest half-extent (Jolt
 *  asserts on that, and generated props are often thin). */
JPH::ShapeRefC safe_box(const Vec3& half_in) {
    const Vec3 half(std::max(0.005f, half_in.x), std::max(0.005f, half_in.y),
                    std::max(0.005f, half_in.z));
    const float r = std::min(0.05f, 0.5f * std::min({half.x, half.y, half.z}));
    return new JPH::BoxShape(JPH::Vec3(half.x, half.y, half.z), r);
}

/** Convex hull from measured hull points, or null when they are degenerate
 *  (a flat decal, a single line). */
JPH::ShapeRefC build_hull_shape(const std::vector<Vec3>& points, const Vec3& half) {
    if (points.size() < 4) return {};
    JPH::Array<JPH::Vec3> pts;
    pts.reserve(points.size());
    for (const Vec3& p : points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        pts.push_back(JPH::Vec3(p.x, p.y, p.z));
    }
    if (pts.size() < 4) return {};
    // A small convex radius keeps a 6 cm grenade the shape it is drawn.
    const float min_half = std::min({half.x, half.y, half.z});
    const float radius = std::clamp(0.2f * min_half, 0.0f, 0.02f);
    JPH::ConvexHullShapeSettings settings(pts, radius);
    JPH::ShapeSettings::ShapeResult res = settings.Create();
    if (res.HasError()) return {};
    return res.Get();
}

/** Static triangle collider from one of a GLB prop's LODs, in the same
 *  metres the renderer draws it in (node transforms, ``unit_scale``). */
JPH::ShapeRefC build_glb_mesh_shape(const std::string& path, float target_scale_meters,
                                    float unit_scale) {
    std::vector<gltf_mesh_load::LitVertex> verts;
    std::vector<std::uint32_t> indices;
    std::string err;
    if (!gltf_mesh_load::load_glb_merged(path, target_scale_meters, verts, indices, err,
                                         unit_scale)) {
        PTV_WARN("Physics: could not load collision mesh {}: {}", path, err);
        return {};
    }
    JPH::VertexList jverts;
    jverts.reserve(static_cast<JPH::uint>(verts.size()));
    for (const auto& v : verts) jverts.push_back(JPH::Float3(v.pos[0], v.pos[1], v.pos[2]));
    JPH::IndexedTriangleList tris;
    tris.reserve(static_cast<JPH::uint>(indices.size() / 3));
    const std::size_t vcount = verts.size();
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= vcount || b >= vcount || c >= vcount) continue;
        if (a == b || b == c || a == c) continue;
        tris.push_back(JPH::IndexedTriangle(a, b, c, 0));
    }
    if (tris.empty()) return {};
    JPH::MeshShapeSettings settings(jverts, tris);
    settings.Sanitize();
    JPH::ShapeSettings::ShapeResult res = settings.Create();
    if (res.HasError()) return {};
    return res.Get();
}

/** Height to build a character capsule from, or 0 when unknown. */
float capsule_height_for(const World& world, entt::entity entity,
                         const Transform& t) {
    const auto* cm = world.registry().try_get<CMesh>(entity);
    if (cm != nullptr) {
        // Longest axis == height for an upright character, and that is what
        // the loader scales the mesh to.
        if (cm->target_scale_meters > 1.0e-4f) return cm->target_scale_meters;
        if (cm->bounds_radius_m > 1.0e-4f) return 2.0f * cm->bounds_radius_m;
    }
    return std::max(0.20f, t.scale);
}

}  // namespace

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2};
    BPLayerInterface bp_layers{};
    ObjectVsBPLayerFilter obj_vs_bp{};
    ObjectLayerPairFilter obj_pair{};
    JPH::PhysicsSystem system{};
    std::unordered_map<std::uint32_t, JPH::BodyID> entity_to_body{};
    // Reverse lookup so a query result can name the entity it hit. Without
    // it a raycast could only report a point in space, which is useless to a
    // mechanic that needs to know WHAT it shot. Keyed on the BodyID's packed
    // index+sequence rather than BodyID itself, which has no std::hash.
    std::unordered_map<std::uint32_t, std::uint32_t> body_to_entity{};
    // Static GLB triangle colliders, keyed by file + scale: every copy of an
    // asset shares one shape (fourteen range targets build it once).
    std::unordered_map<std::string, JPH::ShapeRefC> glb_mesh_shapes{};
    // Moving bodies floor probes may stand on (BodyID index+sequence).
    std::unordered_set<std::uint32_t> walkable{};
    // True while rebuild_from_world creates the scene's bodies.
    bool building_scene{false};
    // Kinematic poses requested since the last step, applied by step() with
    // the step's own dt. Jolt derives a velocity from target and dt, so a
    // target computed with one frame's dt and integrated over the next
    // frame's overshoots by their ratio: a short frame before a load hitch
    // turned a few centimetres of ground snap into 500 m/s.
    struct KinematicTarget {
        Vec3 position;
        Quat rotation;
    };
    std::unordered_map<std::uint32_t, KinematicTarget> kinematic_targets{};
    // Entities whose body a target moved in the last step; one that gets no
    // new target is stopped instead of coasting on that velocity.
    std::unordered_set<std::uint32_t> kinematic_moving{};
    // The player's part bodies: which group (0 left hand, 1 right hand,
    // 2 legs) each belongs to, and what each group passes through this frame.
    std::unordered_map<std::uint32_t, int> part_group{};
    std::vector<std::unordered_set<std::uint32_t>> part_ignore{3};
    PartContactFilter contact_filter{};

    /** The body shape for a GLB prop from its measured geometry:
     *  triangles for a static ``"mesh"``, the convex hull for anything that
     *  moves (Jolt mesh shapes cannot), an offset box for ``"aabb"``, an
     *  offset sphere for ``"sphere"``. */
    JPH::ShapeRefC measured_shape(const CCollisionGeom& g, const CMesh& cm,
                                  const std::string& kind, JPH::EMotionType motion) {
        const Vec3 half = 0.5f * (g.aabb_max - g.aabb_min);
        const Vec3 center = 0.5f * (g.aabb_max + g.aabb_min);
        if (kind == "mesh" && motion == JPH::EMotionType::Static && g.mesh_lod >= 0) {
            const std::string& path =
                g.mesh_lod < static_cast<int>(cm.lod_paths.size())
                    ? cm.lod_paths[static_cast<std::size_t>(g.mesh_lod)]
                    : cm.glb_path;
            char key_scale[32];
            std::snprintf(key_scale, sizeof(key_scale), "|%.7g|%.7g", cm.unit_scale,
                          cm.target_scale_meters);
            const std::string key = path + key_scale;
            auto it = glb_mesh_shapes.find(key);
            if (it == glb_mesh_shapes.end()) {
                it = glb_mesh_shapes
                         .emplace(key, build_glb_mesh_shape(path, cm.target_scale_meters,
                                                            cm.unit_scale))
                         .first;
            }
            if (it->second.GetPtr() != nullptr) return it->second;
        }
        if (kind == "mesh" || kind == "hull" || kind == "convex") {
            JPH::ShapeRefC hull = build_hull_shape(g.hull, half);
            if (hull.GetPtr() != nullptr) return hull;
        }
        if (kind == "sphere") {
            const float r = std::max(0.005f, std::max({half.x, half.y, half.z}));
            return offset_shape(new JPH::SphereShape(r), center);
        }
        return offset_shape(safe_box(half), center);
    }

    // --- Player character ---
    PlayerContactListener player_listener{system};
    PlayerSweepLayerFilter player_sweep_layers{};
    JPH::Ref<JPH::CharacterVirtual> character;
    float character_height{1.7f};
    float character_radius{0.25f};
    float character_vertical_speed{0.0f};
    std::vector<std::uint32_t> character_ignored;
    IgnoreBodiesFilter character_filter;

    void refresh_character_filter() {
        character_filter.ids.clear();
        for (const std::uint32_t e : character_ignored) {
            const auto it = entity_to_body.find(e);
            if (it != entity_to_body.end()) {
                character_filter.ids.push_back(it->second);
            }
        }
    }
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::initialize() {
    if (impl_) return true;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = jolt_trace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = jolt_assert_failed;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    impl_ = std::make_unique<Impl>();
    impl_->system.Init(
        /*maxBodies=*/2048,
        /*numBodyMutexes=*/0,
        /*maxBodyPairs=*/4096,
        /*maxContactConstraints=*/1024,
        impl_->bp_layers,
        impl_->obj_vs_bp,
        impl_->obj_pair);
    impl_->system.SetGravity(JPH::Vec3{0.0f, -9.81f, 0.0f});
    impl_->contact_filter.part_group = &impl_->part_group;
    impl_->contact_filter.part_ignore = &impl_->part_ignore;
    impl_->system.SetContactListener(&impl_->contact_filter);
    PTV_INFO("Jolt physics initialized");
    return true;
}

void PhysicsWorld::shutdown() {
    if (!impl_) return;
    impl_.reset();
    JPH::UnregisterTypes();
    if (JPH::Factory::sInstance) {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

void PhysicsWorld::rebuild_from_world(World& world) {
    if (!impl_) return;
    auto& bi = impl_->system.GetBodyInterface();
    for (auto [eid, body_id] : impl_->entity_to_body) {
        if (bi.IsAdded(body_id)) bi.RemoveBody(body_id);
        bi.DestroyBody(body_id);
    }
    impl_->entity_to_body.clear();
    impl_->body_to_entity.clear();
    impl_->walkable.clear();
    impl_->part_group.clear();
    impl_->kinematic_targets.clear();
    impl_->kinematic_moving.clear();
    for (auto& s : impl_->part_ignore) s.clear();

    auto view = world.registry().view<CTransform, CPhysics>();
    impl_->building_scene = true;
    for (auto entity : view) {
        add_body_for(world, static_cast<std::uint32_t>(entity));
    }
    impl_->building_scene = false;
    impl_->system.OptimizeBroadPhase();
    PTV_INFO("Physics world rebuilt ({} bodies)", impl_->entity_to_body.size());
}

bool PhysicsWorld::add_body_for(World& world, std::uint32_t entity_id) {
    if (!impl_) return false;
    const auto entity = static_cast<entt::entity>(entity_id);
    if (!world.registry().valid(entity)) return false;
    if (impl_->entity_to_body.count(entity_id) != 0) return true;
    if (!world.registry().all_of<CTransform, CPhysics>(entity)) return false;
    // Spawn templates are parked off-map and hidden from the renderer; giving
    // them colliders too would leave invisible obstacles lying around.
    if (has_tag(world, entity_id, "template")) return false;

    auto& bi = impl_->system.GetBodyInterface();
    const auto& t = world.registry().get<CTransform>(entity).t;
    const auto& p = world.registry().get<CPhysics>(entity);

    JPH::EMotionType motion = JPH::EMotionType::Static;
    JPH::ObjectLayer layer = Layers::STATIC;
    if (p.kind == "dynamic") {
        motion = JPH::EMotionType::Dynamic;
        layer = Layers::PROP;
    } else if (p.kind == "kinematic") {
        motion = JPH::EMotionType::Kinematic;
        layer = Layers::MOVING;
    }

    JPH::ShapeRefC shape_ref;
    bool is_capsule = false;

    // 1) Inline procedural mesh -> triangle mesh collision (static only;
    //    Jolt mesh shapes cannot be dynamic/kinematic).
    if (motion == JPH::EMotionType::Static && p.collision_shape == "mesh") {
        const CInlineMesh* im = world.registry().try_get<CInlineMesh>(entity);
        if (im != nullptr) {
            shape_ref = build_inline_mesh_shape(*im);
        }
    }

    // 2) Capsule -> upright character collider. Checked before CPrimitive so
    //    an authored capsule wins over a primitive's box extents.
    if (shape_ref.GetPtr() == nullptr && p.collision_shape == "capsule") {
        shape_ref = build_capsule_shape(capsule_height_for(world, entity, t),
                                        p.capsule_radius_m);
        is_capsule = shape_ref.GetPtr() != nullptr;
    }

    // 3) Primitive -> non-uniform box / sphere sized by its full extents.
    if (shape_ref.GetPtr() == nullptr) {
        const CPrimitive* prim = world.registry().try_get<CPrimitive>(entity);
        if (prim != nullptr) {
            if (p.collision_shape == "sphere" || prim->kind == "sphere") {
                shape_ref = new JPH::SphereShape(
                    std::max(0.05f, 0.5f * prim->size.x));
            } else {
                JPH::Vec3 half{std::max(0.02f, 0.5f * prim->size.x),
                               std::max(0.02f, 0.5f * prim->size.y),
                               std::max(0.02f, 0.5f * prim->size.z)};
                shape_ref = new JPH::BoxShape(half);
            }
        }
    }

    // 4) A GLB prop: the collider measured from its own mesh (asset
    //    pipeline), in entity space, then scaled by the transform exactly as
    //    the drawn mesh is.
    const CMesh* cm = world.registry().try_get<CMesh>(entity);
    const bool glb = cm != nullptr && !cm->glb_path.empty();
    if (shape_ref.GetPtr() == nullptr && glb) {
        const CCollisionGeom* cg = world.registry().try_get<CCollisionGeom>(entity);
        if (cg != nullptr) {
            shape_ref = impl_->measured_shape(*cg, *cm, p.collision_shape, motion);
        } else {
            // No measurement (a scene compiled before colliders were
            // measured): a cube inscribed in the bounding sphere, so the
            // collider is at least the object's size rather than a fixed 0.5 m.
            const float r = cm->bounds_radius_m > 1e-4f
                                ? cm->bounds_radius_m
                                : 0.5f * std::max(0.05f, cm->target_scale_meters);
            const float h = r * 0.57735f;
            shape_ref = p.collision_shape == "sphere" ? JPH::ShapeRefC(new JPH::SphereShape(r))
                                                      : safe_box(Vec3(h));
            PTV_WARN("Physics: '{}' has no measured collider; using its bounds "
                     "(re-run scene_compile to measure it)", cm->asset_id);
        }
        const float s = t.scale;
        if (shape_ref.GetPtr() != nullptr && std::isfinite(s) && s > 1e-4f &&
            std::abs(s - 1.0f) > 1e-4f) {
            shape_ref = new JPH::ScaledShape(shape_ref, JPH::Vec3::sReplicate(s));
        }
    }

    // 5) Fallback: legacy uniform-scale box / sphere (starter room etc.).
    if (shape_ref.GetPtr() == nullptr) {
        if (p.collision_shape == "sphere") {
            shape_ref = new JPH::SphereShape(std::max(0.01f, 0.5f * t.scale));
        } else {
            shape_ref = safe_box(Vec3(0.25f * t.scale));
        }
    }
    // What the body IS: carried by something, debris, a character, or a prop.
    if (motion != JPH::EMotionType::Static) {
        if (world.registry().all_of<CAttachment>(entity)) {
            layer = Layers::HELD;
        } else if (has_tag(world, entity_id, "debris")) {
            layer = Layers::DEBRIS;
        } else if (is_capsule && has_tag(world, entity_id, "player")) {
            layer = Layers::PLAYER_BODY;
        } else if (is_capsule) {
            layer = Layers::MOVING;
        }
    }

    // Sanitize the transform before handing it to Jolt. Generated/authored
    // entities occasionally carry a non-normalized or zero quaternion, or a
    // non-finite position. Jolt asserts IsNormalized()/IsFinite() inside
    // Body::SetPositionAndRotationInternal, which aborts with SIGTRAP in
    // assert-enabled (debug) builds -- e.g. the Quest debug APK -- and is
    // undefined behavior in release. Fall back to a safe value.
    JPH::RVec3 body_pos{t.position.x, t.position.y, t.position.z};
    if (!std::isfinite(t.position.x) || !std::isfinite(t.position.y) ||
        !std::isfinite(t.position.z)) {
        PTV_WARN("Physics: entity {} has non-finite position; using origin",
                 entity_id);
        body_pos = JPH::RVec3::sZero();
    }
    JPH::Quat body_rot{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w};
    const float rot_len = body_rot.Length();
    if (!std::isfinite(rot_len) || rot_len < 1.0e-6f) {
        PTV_WARN("Physics: entity {} has degenerate rotation; using identity",
                 entity_id);
        body_rot = JPH::Quat::sIdentity();
    } else if (!body_rot.IsNormalized()) {
        body_rot = body_rot.Normalized();
    }

    JPH::BodyCreationSettings bcs(shape_ref, body_pos, body_rot, motion, layer);
    bcs.mRestitution = p.restitution;
    bcs.mFriction = p.friction;
    if (motion != JPH::EMotionType::Static) {
        // Same class of hazard as the transform above, and the one that
        // actually shipped: a scene author writing mass 0 for a KINEMATIC body
        // is describing "nothing pushes this around", which is true and
        // reasonable -- but CalculateInertia divides by the mass, and Jolt
        // asserts mass > 0 inside MotionProperties::SetMassProperties. In the
        // Quest debug APK that assert is a breakpoint, so three zero-mass
        // popup targets killed the process during scene load with no message
        // anywhere: the headset simply returned to the home environment.
        //
        // Substituting a value is safe precisely BECAUSE the author meant
        // "immovable": a kinematic body is driven by its transform and never
        // accelerated by a force, so its mass is not observable. A dynamic
        // body with no mass is a contradiction, and 1 kg is a better guess
        // than a crash.
        float mass_kg = p.mass_kg;
        if (!std::isfinite(mass_kg) || mass_kg <= 0.0f) {
            PTV_WARN(
                "Physics: entity {} is '{}' with mass {} kg; using {} kg. A "
                "movable body needs a positive mass -- use kind 'static' for "
                "something that never moves.",
                entity_id, p.kind, p.mass_kg, kFallbackMassKg);
            mass_kg = kFallbackMassKg;
        }
        bcs.mMassPropertiesOverride.mMass = mass_kg;
        bcs.mOverrideMassProperties =
            JPH::EOverrideMassProperties::CalculateInertia;
    }
    if (is_capsule && motion == JPH::EMotionType::Dynamic) {
        // A character must not topple. Without this a dynamic capsule tips
        // over on the first contact and the NPC animates lying on its side.
        bcs.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX |
                           JPH::EAllowedDOFs::TranslationY |
                           JPH::EAllowedDOFs::TranslationZ |
                           JPH::EAllowedDOFs::RotationY;
    }
    // A prop the layout resolver set down on a surface (tag ``settled``)
    // starts ASLEEP: it stays exactly there until something touches it (a
    // hand, a shot, a blast) instead of jittering on the first frames.
    // Anything else -- spawned at runtime, or authored in the air -- falls.
    const bool settle = impl_->building_scene && motion == JPH::EMotionType::Dynamic &&
                        layer == Layers::PROP && has_tag(world, entity_id, "settled");
    JPH::BodyID id = bi.CreateAndAddBody(
        bcs, settle ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        PTV_WARN("Physics: Jolt refused a body for entity {} (body limit?)", entity_id);
        return false;
    }
    impl_->entity_to_body[entity_id] = id;
    impl_->body_to_entity[id.GetIndexAndSequenceNumber()] = entity_id;
    if (motion != JPH::EMotionType::Static && has_tag(world, entity_id, "walkable")) {
        impl_->walkable.insert(id.GetIndexAndSequenceNumber());
    }
    return true;
}

void PhysicsWorld::set_body_class(std::uint32_t entity, BodyClass body_class) {
    if (!impl_) return;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    // A static body cannot leave the static layer (it has no motion), and
    // nothing movable may enter it.
    const bool is_static = bi.GetMotionType(it->second) == JPH::EMotionType::Static;
    if (is_static != (body_class == BodyClass::Static)) return;
    bi.SetObjectLayer(it->second, layer_for(body_class));
}

PhysicsWorld::BodyClass PhysicsWorld::body_class(std::uint32_t entity) const {
    if (!impl_) return BodyClass::Moving;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return BodyClass::Moving;
    return class_for(impl_->system.GetBodyInterface().GetObjectLayer(it->second));
}

void PhysicsWorld::set_walkable(std::uint32_t entity, bool walkable) {
    if (!impl_) return;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    const std::uint32_t key = it->second.GetIndexAndSequenceNumber();
    if (walkable) impl_->walkable.insert(key);
    else impl_->walkable.erase(key);
}

float PhysicsWorld::body_mass(std::uint32_t entity) const {
    if (!impl_) return 0.0f;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return 0.0f;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), it->second);
    if (!lock.Succeeded()) return 0.0f;
    const JPH::Body& body = lock.GetBody();
    if (!body.IsDynamic() || body.GetMotionProperties() == nullptr) return 0.0f;
    const float inv = body.GetMotionProperties()->GetInverseMass();
    return inv > 1e-9f ? 1.0f / inv : 0.0f;
}

void PhysicsWorld::sleep_body(std::uint32_t entity) {
    if (!impl_) return;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    impl_->system.GetBodyInterface().DeactivateBody(it->second);
}

bool PhysicsWorld::body_active(std::uint32_t entity) const {
    if (!impl_) return false;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return false;
    return impl_->system.GetBodyInterface().IsActive(it->second);
}

void PhysicsWorld::remove_body(std::uint32_t entity_id) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity_id);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    if (bi.IsAdded(it->second)) bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);
    impl_->body_to_entity.erase(it->second.GetIndexAndSequenceNumber());
    impl_->walkable.erase(it->second.GetIndexAndSequenceNumber());
    impl_->part_group.erase(it->second.GetIndexAndSequenceNumber());
    impl_->kinematic_targets.erase(entity_id);
    impl_->kinematic_moving.erase(entity_id);
    impl_->entity_to_body.erase(it);
}

bool PhysicsWorld::create_part_body(std::uint32_t entity, const Vec3& pos, const Quat& rot,
                                    float half_height, float radius, int group) {
    if (!impl_) return false;
    if (impl_->entity_to_body.count(entity) != 0) return true;
    if (!is_finite_vec(pos)) return false;
    const float r = std::clamp(radius, 0.004f, 0.2f);
    const float hh = std::clamp(half_height, 0.002f, 1.0f);
    JPH::ShapeRefC shape = new JPH::CapsuleShape(hh, r);
    JPH::Quat q{rot.x, rot.y, rot.z, rot.w};
    if (!std::isfinite(q.Length()) || q.Length() < 1e-6f) q = JPH::Quat::sIdentity();
    q = q.Normalized();
    JPH::BodyCreationSettings bcs(shape, JPH::RVec3{pos.x, pos.y, pos.z}, q,
                                  JPH::EMotionType::Kinematic, Layers::PLAYER_PART);
    bcs.mFriction = 0.8f;
    bcs.mRestitution = 0.0f;
    bcs.mMassPropertiesOverride.mMass = 0.3f;
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    auto& bi = impl_->system.GetBodyInterface();
    const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
    if (id.IsInvalid()) return false;
    impl_->entity_to_body[entity] = id;
    impl_->body_to_entity[id.GetIndexAndSequenceNumber()] = entity;
    impl_->part_group[id.GetIndexAndSequenceNumber()] = group;
    return true;
}

void PhysicsWorld::set_part_ignore(int group, const std::vector<std::uint32_t>& entities) {
    if (!impl_ || group < 0 || group >= static_cast<int>(impl_->part_ignore.size())) return;
    auto& set = impl_->part_ignore[static_cast<std::size_t>(group)];
    set.clear();
    for (const std::uint32_t e : entities) {
        const auto it = impl_->entity_to_body.find(e);
        if (it != impl_->entity_to_body.end()) set.insert(it->second.GetIndexAndSequenceNumber());
    }
}

void PhysicsWorld::set_collision_enabled(std::uint32_t entity, bool enabled) {
    if (!impl_) return;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    const bool added = bi.IsAdded(it->second);
    if (enabled && !added) {
        bi.AddBody(it->second, JPH::EActivation::Activate);
    } else if (!enabled && added) {
        bi.RemoveBody(it->second);
    }
}

float PhysicsWorld::distance_to_body(std::uint32_t entity, const Vec3& point) const {
    if (!impl_) return -1.0f;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return -1.0f;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), it->second);
    if (!lock.Succeeded()) return -1.0f;
    const JPH::Body& body = lock.GetBody();
    // The point in the shape's own (centre-of-mass) frame, against the
    // shape's local bounds: an oriented box around the collider, so a long
    // rifle is reachable at its stock as well as at its middle.
    const JPH::RMat44 com = body.GetCenterOfMassTransform();
    const JPH::Vec3 local = JPH::Vec3(com.InversedRotationTranslation() *
                                      JPH::RVec3(point.x, point.y, point.z));
    const JPH::AABox box = body.GetShape()->GetLocalBounds();
    const JPH::Vec3 clamped = JPH::Vec3::sMin(JPH::Vec3::sMax(local, box.mMin), box.mMax);
    return (local - clamped).Length();
}

void PhysicsWorld::overlapping(std::uint32_t entity_id,
                               std::vector<std::uint32_t>& out) const {
    out.clear();
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity_id);
    if (it == impl_->entity_to_body.end()) return;

    // Snapshot the shape and pose under a read lock, then release it before
    // querying: CollideShape takes its own locks and would deadlock.
    JPH::RefConst<JPH::Shape> shape;
    JPH::RMat44 com_transform;
    {
        JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(),
                               it->second);
        if (!lock.Succeeded()) return;
        const JPH::Body& body = lock.GetBody();
        shape = body.GetShape();
        com_transform = body.GetCenterOfMassTransform();
    }
    if (shape == nullptr) return;

    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = 0.0f;

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    // sReplicate(1) rather than sOne(): Jolt 5.2.0 (the version the Android
    // NDK build fetches) dropped sOne, and desktop vcpkg still has it, so
    // sOne compiles here and breaks only the Quest APK.
    impl_->system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1.0f), com_transform, settings,
        com_transform.GetTranslation(), collector, {}, {},
        JPH::IgnoreSingleBodyFilter(it->second));

    for (const auto& hit : collector.mHits) {
        auto found =
            impl_->body_to_entity.find(hit.mBodyID2.GetIndexAndSequenceNumber());
        if (found == impl_->body_to_entity.end()) continue;
        if (found->second == entity_id) continue;
        // CollideShape can report several contact points per body; gameplay
        // only cares that the pair touches at all.
        if (std::find(out.begin(), out.end(), found->second) != out.end())
            continue;
        out.push_back(found->second);
    }
}

void PhysicsWorld::step(float dt) {
    if (!impl_) return;
    if (!(dt > 0.0f)) return;
    auto& bi = impl_->system.GetBodyInterface();
    const auto kinematic = [&](std::uint32_t entity, JPH::BodyID* out) {
        const auto it = impl_->entity_to_body.find(entity);
        if (it == impl_->entity_to_body.end()) return false;
        if (bi.GetMotionType(it->second) != JPH::EMotionType::Kinematic) return false;
        *out = it->second;
        return true;
    };
    for (const std::uint32_t entity : impl_->kinematic_moving) {
        JPH::BodyID id;
        if (impl_->kinematic_targets.count(entity) == 0 && kinematic(entity, &id)) {
            bi.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
    }
    impl_->kinematic_moving.clear();
    for (const auto& [entity, target] : impl_->kinematic_targets) {
        JPH::BodyID id;
        if (!kinematic(entity, &id)) continue;
        bi.MoveKinematic(id, JPH::RVec3{target.position.x, target.position.y, target.position.z},
                         JPH::Quat{target.rotation.x, target.rotation.y, target.rotation.z,
                                   target.rotation.w},
                         dt);
        impl_->kinematic_moving.insert(entity);
    }
    impl_->kinematic_targets.clear();
    const int substeps = 1;
    impl_->system.Update(dt, substeps, &impl_->temp, &impl_->jobs);
}

void PhysicsWorld::sync_to_world(World& world) {
    if (!impl_) return;
    auto& bi = impl_->system.GetBodyInterface();
    for (auto& [eid, body_id] : impl_->entity_to_body) {
        auto entity = static_cast<entt::entity>(eid);
        if (!world.registry().valid(entity)) continue;
        auto& t = world.registry().get<CTransform>(entity).t;
        // The body's ORIGIN, not its centre of mass: a measured collider is
        // offset from the mesh origin (a centred table, a hull whose
        // centroid is not the pivot), and writing the COM back would shift
        // the drawn mesh by that offset.
        JPH::RVec3 p{};
        JPH::Quat q{};
        bi.GetPositionAndRotation(body_id, p, q);
        t.position = Vec3{(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()};
        t.rotation = Quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }
}

bool PhysicsWorld::has_body(std::uint32_t entity) const {
    if (!impl_) return false;
    return impl_->entity_to_body.find(entity) != impl_->entity_to_body.end();
}

bool PhysicsWorld::body_center(std::uint32_t entity, Vec3* out) const {
    if (!impl_ || out == nullptr) return false;
    const auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return false;
    const JPH::AABox box =
        impl_->system.GetBodyInterface().GetTransformedShape(it->second).GetWorldSpaceBounds();
    const JPH::Vec3 c = box.GetCenter();
    *out = Vec3(c.GetX(), c.GetY(), c.GetZ());
    return true;
}

bool PhysicsWorld::recreate_body_movable(std::uint32_t entity,
                                         int motion_type_raw) {
    // Replace a static body with an otherwise identical movable one. Jolt
    // decides at creation whether to allocate MotionProperties, so the motion
    // type of an existing body can only be changed within the movable family.
    if (!impl_) return false;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return false;

    auto& bi = impl_->system.GetBodyInterface();
    const JPH::BodyID old_id = it->second;
    const auto motion = static_cast<JPH::EMotionType>(motion_type_raw);

    JPH::ShapeRefC shape = bi.GetShape(old_id);
    if (shape.GetPtr() == nullptr) return false;
    JPH::RVec3 pos{};
    JPH::Quat rot{};
    bi.GetPositionAndRotation(old_id, pos, rot);
    const float friction = bi.GetFriction(old_id);
    const float restitution = bi.GetRestitution(old_id);

    remove_body(entity);

    // A movable layer, or the new body keeps colliding by static rules.
    JPH::BodyCreationSettings bcs(
        shape, pos, rot, motion,
        motion == JPH::EMotionType::Dynamic ? Layers::PROP : Layers::MOVING);
    bcs.mFriction = friction;
    bcs.mRestitution = restitution;
    bcs.mMassPropertiesOverride.mMass = kFallbackMassKg;
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;

    const JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        PTV_WARN("Physics: could not recreate entity {} as a movable body",
                 entity);
        return false;
    }
    impl_->entity_to_body[entity] = id;
    impl_->body_to_entity[id.GetIndexAndSequenceNumber()] = entity;
    PTV_INFO("Physics: entity {} promoted from static to a movable body", entity);
    return true;
}

void PhysicsWorld::set_kinematic(std::uint32_t entity, bool kinematic) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    const JPH::EMotionType target = kinematic ? JPH::EMotionType::Kinematic
                                              : JPH::EMotionType::Dynamic;

    // A static body has no MotionProperties -- Jolt allocates them only for
    // movable bodies -- so SetMotionType cannot promote one. It walks
    // straight into the null pointer: ActivateBodies dereferences it and the
    // process takes a SIGSEGV at offset 0x80, during scene load, with nothing
    // logged. (A shipped APK died exactly this way, on a generated mechanic
    // calling set_kinematic on a static target to raise and lower it.)
    //
    // Recreating the body is the honest fix. Refusing would be safe but would
    // silently break the mechanic, and "this prop should start moving now" is
    // a reasonable thing for generated gameplay code to decide about scenery
    // the scene author had no reason to mark movable.
    if (bi.GetMotionType(it->second) == JPH::EMotionType::Static) {
        if (!recreate_body_movable(entity, static_cast<int>(target))) return;
        it = impl_->entity_to_body.find(entity);
        if (it == impl_->entity_to_body.end()) return;
    }

    bi.SetMotionType(it->second, target, JPH::EActivation::Activate);
    // Held while kinematic: a carried prop must not touch its holder.
    const JPH::ObjectLayer layer = bi.GetObjectLayer(it->second);
    if (kinematic && layer == Layers::PROP) {
        bi.SetObjectLayer(it->second, Layers::HELD);
    } else if (!kinematic && layer == Layers::HELD) {
        bi.SetObjectLayer(it->second, Layers::PROP);
    }
    if (!kinematic) {
        // When returning to Dynamic after a grab, zero any residual
        // kinematic velocity so the body is "dropped" cleanly (callers
        // can then apply a throw impulse).
        bi.SetLinearVelocity(it->second, JPH::Vec3{0.0f, 0.0f, 0.0f});
        bi.SetAngularVelocity(it->second, JPH::Vec3{0.0f, 0.0f, 0.0f});
    }
}

void PhysicsWorld::move_kinematic(std::uint32_t entity, const Vec3& pos,
                                  const Quat& rot, float dt) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    if (dt <= 0.0f) dt = 1.0f / 90.0f;
    auto& bi = impl_->system.GetBodyInterface();
    // Same null-MotionProperties trap as set_kinematic: MoveKinematic reads
    // the motion properties a static body does not have. Callers reach here
    // through generated code, so refuse rather than fault.
    // Same null-MotionProperties trap as set_kinematic, and the same answer:
    // promote the body rather than fault or refuse. A caller asking to sweep
    // something to a new pose has said plainly that it should move, and
    // generated mechanics reach here without necessarily having called
    // set_kinematic first. Promotion happens once; afterwards this is a
    // straight MoveKinematic.
    if (bi.GetMotionType(it->second) == JPH::EMotionType::Static) {
        if (!recreate_body_movable(
                entity, static_cast<int>(JPH::EMotionType::Kinematic))) {
            return;
        }
        it = impl_->entity_to_body.find(entity);
        if (it == impl_->entity_to_body.end()) return;
    }
    (void)dt;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
    impl_->kinematic_targets[entity] = Impl::KinematicTarget{pos, rot};
}

void PhysicsWorld::teleport(std::uint32_t entity, const Vec3& pos,
                             const Quat& rot) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    impl_->kinematic_targets.erase(entity);
    bi.SetPositionAndRotation(it->second,
                              JPH::RVec3{pos.x, pos.y, pos.z},
                              JPH::Quat{rot.x, rot.y, rot.z, rot.w},
                              JPH::EActivation::Activate);
    if (bi.GetMotionType(it->second) != JPH::EMotionType::Static) {
        bi.SetLinearAndAngularVelocity(it->second, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    }
}

void PhysicsWorld::apply_impulse(std::uint32_t entity, const Vec3& impulse) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    bi.AddImpulse(it->second, JPH::Vec3{impulse.x, impulse.y, impulse.z});
}

Vec3 PhysicsWorld::slide_character(const Vec3& center, float height,
                                   const Vec3& delta,
                                   std::uint32_t ignore_entity) const {
    return slide_character(center, height, delta,
                           std::vector<std::uint32_t>{ignore_entity});
}

Vec3 PhysicsWorld::slide_character(
    const Vec3& center, float height, const Vec3& delta,
    const std::vector<std::uint32_t>& ignore) const {
    if (!impl_) return delta;
    const float want = glm::length(delta);
    if (want < 1.0e-6f) return delta;

    JPH::ShapeRefC shape = build_capsule_shape(height);
    if (shape.GetPtr() == nullptr) return delta;

    // Exclude the mover's own body, otherwise the very first cast reports a
    // zero-distance hit against itself and the player can never move.
    IgnoreBodiesFilter body_filter;
    for (const std::uint32_t e : ignore) {
        if (auto it = impl_->entity_to_body.find(e);
            it != impl_->entity_to_body.end()) {
            body_filter.ids.push_back(it->second);
        }
    }

    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = false;

    Vec3 origin = center;
    Vec3 remaining = delta;
    Vec3 moved(0.0f);

    // Three iterations: hit a wall, slide along it, and possibly hit a second
    // wall in the corner. A fourth pass buys nothing but frame time.
    for (int iter = 0; iter < 3; ++iter) {
        const float len = glm::length(remaining);
        if (len < 1.0e-5f) break;

        const JPH::RMat44 start = JPH::RMat44::sTranslation(
            JPH::RVec3{origin.x, origin.y, origin.z});
        // sReplicate(1), not sOne -- see the note in overlapping() above.
        const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
            shape, JPH::Vec3::sReplicate(1.0f), start,
            JPH::Vec3{remaining.x, remaining.y, remaining.z});

        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        impl_->system.GetNarrowPhaseQuery().CastShape(
            cast, settings, JPH::RVec3{origin.x, origin.y, origin.z},
            collector, {}, {}, body_filter);

        if (!collector.HadHit()) {
            moved += remaining;
            break;
        }

        // Stop just short of contact so the next frame's cast does not start
        // already touching, which would report fraction 0 forever.
        const float frac = std::clamp(collector.mHit.mFraction, 0.0f, 1.0f);
        const float safe = std::max(0.0f, frac - 0.002f);
        const Vec3 step = remaining * safe;
        moved += step;
        origin += step;

        // The penetration axis points from the mover into the obstacle, so
        // the surface normal we slide along is its negation.
        const JPH::Vec3 axis = collector.mHit.mPenetrationAxis.Normalized();
        Vec3 n(-axis.GetX(), -axis.GetY(), -axis.GetZ());
        const float n_len = glm::length(n);
        if (!std::isfinite(n_len) || n_len < 1.0e-5f) break;
        n /= n_len;

        // Already touching and moving AWAY: the contact is behind us, so it
        // must not stop the move. This is what keeps a wedged mover able to
        // step back out without ever letting it push further in.
        if (frac <= 0.002f && glm::dot(remaining, n) >= 0.0f) {
            moved += remaining;
            break;
        }

        const Vec3 leftover = remaining * (1.0f - safe);
        remaining = leftover - n * glm::dot(leftover, n);
    }

    // Fail closed: whatever the sweep resolved is the answer, even if it is
    // nothing. The move that was refused pointed into a wall.
    (void)want;
    return moved;
}

// ---------------------------------------------------------------------------
// Player character
// ---------------------------------------------------------------------------

void PhysicsWorld::create_player_character(const Vec3& feet, float height,
                                           float radius) {
    if (!impl_) return;
    impl_->character = nullptr;
    impl_->character_height = std::clamp(height, 0.5f, 2.6f);
    impl_->character_radius =
        std::clamp(radius, 0.08f, 0.45f * impl_->character_height);
    impl_->character_vertical_speed = 0.0f;

    JPH::ShapeRefC shape =
        build_feet_capsule(impl_->character_height, impl_->character_radius);
    if (shape.GetPtr() == nullptr) return;

    JPH::Ref<JPH::CharacterVirtualSettings> settings =
        new JPH::CharacterVirtualSettings();
    settings->mShape = shape;
    settings->mUp = JPH::Vec3::sAxisY();
    settings->mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    // Only contacts on the lower hemisphere count as standing on something.
    settings->mSupportingVolume =
        JPH::Plane(JPH::Vec3::sAxisY(), -impl_->character_radius);
    // Collide with BOTH faces of every triangle. Generated worlds are full of
    // single-sided meshes, and a one-sided wall is exactly a wall the player
    // can walk through from behind.
    settings->mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    settings->mCharacterPadding = 0.02f;
    settings->mPenetrationRecoverySpeed = 1.0f;
    settings->mPredictiveContactDistance = 0.1f;
    settings->mMass = 70.0f;
    settings->mMaxStrength = 100.0f;

    impl_->character = new JPH::CharacterVirtual(
        settings, JPH::RVec3(feet.x, feet.y, feet.z), JPH::Quat::sIdentity(),
        0, &impl_->system);
    impl_->character->SetListener(&impl_->player_listener);
}

void PhysicsWorld::destroy_player_character() {
    if (!impl_) return;
    impl_->character = nullptr;
}

bool PhysicsWorld::has_player_character() const {
    return impl_ && impl_->character != nullptr;
}

void PhysicsWorld::set_player_character_ignored(
    const std::vector<std::uint32_t>& entities) {
    if (!impl_) return;
    impl_->character_ignored = entities;
}

Vec3 PhysicsWorld::move_player_character(const Vec3& displacement,
                                         bool grounded, float jump_speed,
                                         float dt, bool apply_gravity) {
    if (!impl_ || impl_->character == nullptr) return displacement;
    if (!std::isfinite(dt) || dt <= 1.0e-5f) dt = 1.0f / 72.0f;
    auto& ch = *impl_->character;
    impl_->refresh_character_filter();

    const JPH::RVec3 before = ch.GetPosition();
    const JPH::Vec3 gravity = impl_->system.GetGravity();

    JPH::Vec3 velocity;
    if (grounded && !apply_gravity) {
        // A second, purely horizontal pass in the same frame (the body
        // following the head). Vertical motion was already integrated.
        velocity = JPH::Vec3(displacement.x / dt, 0.0f, displacement.z / dt);
    } else if (grounded) {
        float vy = impl_->character_vertical_speed;
        const bool on_ground =
            ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        if (on_ground && vy <= 0.0f) vy = 0.0f;
        if (on_ground && jump_speed > 0.0f) vy = jump_speed;
        vy += gravity.GetY() * dt;
        // Terminal velocity: a fall through a gap in the level should not
        // accelerate forever before the floor clamp catches it.
        vy = std::max(vy, -30.0f);
        velocity = JPH::Vec3(displacement.x / dt, vy, displacement.z / dt);
        if (on_ground) velocity += ch.GetGroundVelocity();
    } else {
        velocity = JPH::Vec3(displacement.x / dt, displacement.y / dt,
                             displacement.z / dt);
    }
    ch.SetLinearVelocity(velocity);

    JPH::CharacterVirtual::ExtendedUpdateSettings ext;
    const bool rising = grounded && velocity.GetY() > 0.0f;
    ext.mStickToFloorStepDown =
        (grounded && !rising) ? JPH::Vec3(0.0f, -0.5f, 0.0f)
                              : JPH::Vec3::sZero();
    ext.mWalkStairsStepUp =
        grounded ? JPH::Vec3(0.0f, 0.35f, 0.0f) : JPH::Vec3::sZero();

    ch.ExtendedUpdate(
        dt, grounded ? gravity : JPH::Vec3::sZero(), ext, {},
        impl_->player_sweep_layers, impl_->character_filter, {}, impl_->temp);

    if (grounded && !apply_gravity) {
        // Leave the integrated vertical speed alone.
    } else if (grounded) {
        const bool supported = ch.IsSupported();
        const float vy = ch.GetLinearVelocity().GetY();
        impl_->character_vertical_speed = (supported && vy <= 0.0f) ? 0.0f : vy;
    } else {
        impl_->character_vertical_speed = 0.0f;
    }

    const JPH::RVec3 after = ch.GetPosition();
    return Vec3(static_cast<float>(after.GetX() - before.GetX()),
                static_cast<float>(after.GetY() - before.GetY()),
                static_cast<float>(after.GetZ() - before.GetZ()));
}

void PhysicsWorld::player_character_contacts(
    std::vector<std::uint32_t>& out) const {
    out.clear();
    if (!impl_ || impl_->character == nullptr) return;
    for (const auto& c : impl_->character->GetActiveContacts()) {
        if (!c.mHadCollision || c.mIsSensorB || c.mBodyB.IsInvalid()) continue;
        const auto it =
            impl_->body_to_entity.find(c.mBodyB.GetIndexAndSequenceNumber());
        if (it == impl_->body_to_entity.end()) continue;
        if (std::find(out.begin(), out.end(), it->second) == out.end()) {
            out.push_back(it->second);
        }
    }
}

void PhysicsWorld::set_player_character_position(const Vec3& feet) {
    if (!impl_ || impl_->character == nullptr) return;
    impl_->character->SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
}

void PhysicsWorld::teleport_player_character(const Vec3& feet) {
    if (!impl_ || impl_->character == nullptr) return;
    impl_->character->SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
    impl_->character->SetLinearVelocity(JPH::Vec3::sZero());
    impl_->character_vertical_speed = 0.0f;
}

bool PhysicsWorld::resize_player_character(float height) {
    if (!impl_ || impl_->character == nullptr) return false;
    const float h = std::clamp(height, 0.5f, 2.6f);
    if (std::abs(h - impl_->character_height) < 1.0e-3f) return true;
    JPH::ShapeRefC shape = build_feet_capsule(h, impl_->character_radius);
    if (shape.GetPtr() == nullptr) return false;
    impl_->refresh_character_filter();
    const bool ok = impl_->character->SetShape(
        shape, 1.5f * impl_->character->GetCharacterPadding(), {},
        impl_->player_sweep_layers, impl_->character_filter, {}, impl_->temp);
    if (ok) impl_->character_height = h;
    return ok;
}

PhysicsWorld::CharacterState PhysicsWorld::player_character_state() const {
    CharacterState s;
    if (!impl_ || impl_->character == nullptr) return s;
    const auto& ch = *impl_->character;
    const JPH::RVec3 p = ch.GetPosition();
    s.feet = Vec3(static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                  static_cast<float>(p.GetZ()));
    s.grounded = ch.IsSupported();
    const JPH::Vec3 n = ch.GetGroundNormal();
    s.ground_normal = Vec3(n.GetX(), n.GetY(), n.GetZ());
    s.vertical_speed = impl_->character_vertical_speed;
    s.height = impl_->character_height;
    const JPH::Vec3 gv = ch.GetGroundVelocity();
    s.ground_velocity = Vec3(gv.GetX(), gv.GetY(), gv.GetZ());
    return s;
}

Vec3 PhysicsWorld::resolve_head(const Vec3& from, const Vec3& to,
                                float radius) const {
    if (!impl_) return to;
    if (!is_finite_vec(from) || !is_finite_vec(to)) return to;
    const float r = std::clamp(radius, 0.03f, 0.5f);
    JPH::ShapeRefC sphere = new JPH::SphereShape(r);

    const JPH::SpecifiedBroadPhaseLayerFilter bp_filter(BPLayers::STATIC);
    const JPH::SpecifiedObjectLayerFilter obj_filter(Layers::STATIC);

    // 1) Sweep, so a fast head movement cannot tunnel through a thin wall.
    Vec3 pos = from;
    const Vec3 delta = to - from;
    if (glm::length(delta) > 1.0e-5f) {
        const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
            sphere, JPH::Vec3::sReplicate(1.0f),
            JPH::RMat44::sTranslation(JPH::RVec3(from.x, from.y, from.z)),
            JPH::Vec3(delta.x, delta.y, delta.z));
        JPH::ShapeCastSettings settings;
        settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
        settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        impl_->system.GetNarrowPhaseQuery().CastShape(
            cast, settings, JPH::RVec3(from.x, from.y, from.z), collector,
            bp_filter, obj_filter);
        if (collector.HadHit() && collector.mHit.mFraction > 0.0f) {
            const float frac =
                std::clamp(collector.mHit.mFraction - 0.01f, 0.0f, 1.0f);
            pos = from + delta * frac;
            // Slide the rest along the wall, so brushing a wall does not
            // pin the head in place.
            const JPH::Vec3 axis = collector.mHit.mPenetrationAxis.Normalized();
            Vec3 n(-axis.GetX(), -axis.GetY(), -axis.GetZ());
            if (glm::length(n) > 1.0e-5f) {
                n = glm::normalize(n);
                Vec3 rest = delta * (1.0f - frac);
                rest -= n * glm::dot(rest, n);
                pos += rest;
            }
        } else if (!collector.HadHit()) {
            pos = to;
        } else {
            pos = to;  // starting in contact; the push-out below handles it
        }
    }

    // 2) Push out of anything still overlapped.
    for (int iter = 0; iter < 3; ++iter) {
        JPH::CollideShapeSettings cs;
        cs.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
        cs.mMaxSeparationDistance = 0.0f;
        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
        const JPH::RMat44 xf =
            JPH::RMat44::sTranslation(JPH::RVec3(pos.x, pos.y, pos.z));
        impl_->system.GetNarrowPhaseQuery().CollideShape(
            sphere, JPH::Vec3::sReplicate(1.0f), xf, cs, xf.GetTranslation(),
            hits, bp_filter, obj_filter);
        if (hits.mHits.empty()) break;
        const JPH::CollideShapeResult* deepest = nullptr;
        for (const auto& h : hits.mHits) {
            if (deepest == nullptr ||
                h.mPenetrationDepth > deepest->mPenetrationDepth) {
                deepest = &h;
            }
        }
        if (deepest == nullptr || deepest->mPenetrationDepth <= 1.0e-4f) break;
        const JPH::Vec3 axis = deepest->mPenetrationAxis.Normalized();
        pos -= Vec3(axis.GetX(), axis.GetY(), axis.GetZ()) *
               (deepest->mPenetrationDepth + 0.002f);
    }
    return pos;
}

bool PhysicsWorld::raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                           Vec3& out_hit, Vec3& out_normal) const {
    std::uint32_t ignored = kInvalidEntity;
    return raycast(origin, dir, max_dist, out_hit, out_normal, ignored);
}

bool PhysicsWorld::raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                           Vec3& out_hit, Vec3& out_normal,
                           std::uint32_t& out_entity) const {
    static const RayFilter kNoFilter{};
    return raycast(origin, dir, max_dist, kNoFilter, out_hit, out_normal,
                   out_entity);
}

namespace {

/** Body filter for :meth:`PhysicsWorld::raycast` with a RayFilter. */
class RayBodyFilter final : public JPH::BodyFilter {
public:
    std::vector<JPH::BodyID> ids;
    bool skip_characters{false};
    bool floor_only{false};
    const std::unordered_set<std::uint32_t>* walkable{nullptr};
    bool ShouldCollide(const JPH::BodyID& id) const override {
        return std::find(ids.begin(), ids.end(), id) == ids.end();
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        if (body.IsSensor()) return false;
        if (floor_only && !body.IsStatic()) {
            return walkable != nullptr &&
                   walkable->count(body.GetID().GetIndexAndSequenceNumber()) != 0;
        }
        if (skip_characters) {
            if (body.GetShape() != nullptr &&
                body.GetShape()->GetSubType() == JPH::EShapeSubType::Capsule) {
                return false;
            }
            const JPH::ObjectLayer l = body.GetObjectLayer();
            if (l == Layers::HELD || l == Layers::PLAYER_BODY || l == Layers::PLAYER_PART) {
                return false;
            }
        }
        return true;
    }
};

}  // namespace

bool PhysicsWorld::raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                           const RayFilter& filter, Vec3& out_hit,
                           Vec3& out_normal, std::uint32_t& out_entity) const {
    out_entity = kInvalidEntity;
    if (!impl_) return false;
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f || max_dist <= 0.0f) return false;
    const JPH::Vec3 d{dir.x / len * max_dist, dir.y / len * max_dist,
                      dir.z / len * max_dist};
    JPH::RRayCast ray{JPH::RVec3{origin.x, origin.y, origin.z}, d};
    JPH::RayCastResult hit;

    RayBodyFilter body_filter;
    body_filter.skip_characters = filter.skip_characters;
    body_filter.floor_only = filter.floor_only;
    body_filter.walkable = &impl_->walkable;
    for (const std::uint32_t e : filter.ignore) {
        if (auto it = impl_->entity_to_body.find(e);
            it != impl_->entity_to_body.end()) {
            body_filter.ids.push_back(it->second);
        }
    }
    bool any_hit = false;
    if (filter.static_only) {
        const JPH::SpecifiedBroadPhaseLayerFilter bp_filter(BPLayers::STATIC);
        const JPH::SpecifiedObjectLayerFilter obj_filter(Layers::STATIC);
        any_hit = impl_->system.GetNarrowPhaseQuery().CastRay(
            ray, hit, bp_filter, obj_filter, body_filter);
    } else if (filter.floor_only) {
        const FloorLayerFilter obj_filter;
        any_hit = impl_->system.GetNarrowPhaseQuery().CastRay(
            ray, hit, {}, obj_filter, body_filter);
    } else {
        any_hit = impl_->system.GetNarrowPhaseQuery().CastRay(
            ray, hit, {}, {}, body_filter);
    }
    if (!any_hit) return false;

    JPH::RVec3 p = ray.GetPointOnRay(hit.mFraction);
    out_hit = Vec3{(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()};
    out_normal = Vec3{0.0f, 1.0f, 0.0f};
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
            hit.mSubShapeID2, p);
        out_normal = Vec3{n.GetX(), n.GetY(), n.GetZ()};
    }
    if (auto found =
            impl_->body_to_entity.find(hit.mBodyID.GetIndexAndSequenceNumber());
        found != impl_->body_to_entity.end()) {
        out_entity = found->second;
    }
    return true;
}

bool PhysicsWorld::sphere_cast(const Vec3& origin, const Vec3& dir, float radius,
                               float max_dist, const RayFilter& filter,
                               Vec3& out_hit, Vec3& out_normal,
                               std::uint32_t& out_entity) const {
    out_entity = kInvalidEntity;
    if (!impl_) return false;
    if (radius <= 1e-4f) {
        return raycast(origin, dir, max_dist, filter, out_hit, out_normal, out_entity);
    }
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f || max_dist <= 0.0f || !is_finite_vec(origin)) return false;
    const Vec3 delta = dir / len * max_dist;
    JPH::ShapeRefC sphere = new JPH::SphereShape(std::clamp(radius, 0.001f, 2.0f));
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        sphere, JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sTranslation(JPH::RVec3(origin.x, origin.y, origin.z)),
        JPH::Vec3(delta.x, delta.y, delta.z));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
    RayBodyFilter body_filter;
    body_filter.skip_characters = filter.skip_characters;
    body_filter.floor_only = filter.floor_only;
    body_filter.walkable = &impl_->walkable;
    for (const std::uint32_t e : filter.ignore) {
        if (auto it = impl_->entity_to_body.find(e); it != impl_->entity_to_body.end()) {
            body_filter.ids.push_back(it->second);
        }
    }
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    if (filter.static_only) {
        const JPH::SpecifiedBroadPhaseLayerFilter bp_filter(BPLayers::STATIC);
        const JPH::SpecifiedObjectLayerFilter obj_filter(Layers::STATIC);
        impl_->system.GetNarrowPhaseQuery().CastShape(
            cast, settings, JPH::RVec3(origin.x, origin.y, origin.z), collector, bp_filter,
            obj_filter, body_filter);
    } else if (filter.floor_only) {
        const FloorLayerFilter obj_filter;
        impl_->system.GetNarrowPhaseQuery().CastShape(
            cast, settings, JPH::RVec3(origin.x, origin.y, origin.z), collector, {},
            obj_filter, body_filter);
    } else {
        impl_->system.GetNarrowPhaseQuery().CastShape(
            cast, settings, JPH::RVec3(origin.x, origin.y, origin.z), collector, {}, {},
            body_filter);
    }
    if (!collector.HadHit()) return false;
    const auto& h = collector.mHit;
    // Contact point on the struck body; the normal points back at the caster.
    out_hit = Vec3(static_cast<float>(h.mContactPointOn2.GetX()),
                   static_cast<float>(h.mContactPointOn2.GetY()),
                   static_cast<float>(h.mContactPointOn2.GetZ()));
    const JPH::Vec3 axis = h.mPenetrationAxis.Normalized();
    out_normal = Vec3(-axis.GetX(), -axis.GetY(), -axis.GetZ());
    if (auto found = impl_->body_to_entity.find(h.mBodyID2.GetIndexAndSequenceNumber());
        found != impl_->body_to_entity.end()) {
        out_entity = found->second;
    }
    return true;
}

}  // namespace ptv
