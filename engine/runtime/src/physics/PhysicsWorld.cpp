#include "ptv/physics/PhysicsWorld.h"

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"

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
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ptv {

namespace {

// Object/broadphase layers: two classes, static and moving.
namespace Layers {
    static constexpr JPH::ObjectLayer STATIC = 0;
    static constexpr JPH::ObjectLayer MOVING = 1;
    static constexpr JPH::ObjectLayer NUM = 2;
}  // namespace Layers
namespace BPLayers {
    static constexpr JPH::BroadPhaseLayer STATIC{0};
    static constexpr JPH::BroadPhaseLayer MOVING{1};
    static constexpr uint32_t NUM = 2;
}  // namespace BPLayers

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterface() {
        map_[Layers::STATIC] = BPLayers::STATIC;
        map_[Layers::MOVING] = BPLayers::MOVING;
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
    bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override {
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        // Static <-> Static do not collide.
        if (a == Layers::STATIC && b == Layers::STATIC) return false;
        return true;
    }
};

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

JPH::ShapeRefC build_capsule_shape(float height_m) {
    const float height = std::max(0.20f, height_m);
    const float radius = std::clamp(0.17f * height, 0.05f, 0.45f * height);
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

    // --- Player character ---
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
        bi.RemoveBody(body_id);
        bi.DestroyBody(body_id);
    }
    impl_->entity_to_body.clear();
    impl_->body_to_entity.clear();

    auto view = world.registry().view<CTransform, CPhysics>();
    for (auto entity : view) {
        add_body_for(world, static_cast<std::uint32_t>(entity));
    }
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
        layer = Layers::MOVING;
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
        shape_ref = build_capsule_shape(capsule_height_for(world, entity, t));
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

    // 4) Fallback: legacy uniform-scale box / sphere (starter room etc.).
    if (shape_ref.GetPtr() == nullptr) {
        if (p.collision_shape == "sphere") {
            shape_ref = new JPH::SphereShape(0.5f * t.scale);
        } else {
            JPH::Vec3 half{0.25f * t.scale, 0.25f * t.scale, 0.25f * t.scale};
            shape_ref = new JPH::BoxShape(half);
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
    JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
    impl_->entity_to_body[entity_id] = id;
    impl_->body_to_entity[id.GetIndexAndSequenceNumber()] = entity_id;
    return true;
}

void PhysicsWorld::remove_body(std::uint32_t entity_id) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity_id);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    bi.RemoveBody(it->second);
    bi.DestroyBody(it->second);
    impl_->body_to_entity.erase(it->second.GetIndexAndSequenceNumber());
    impl_->entity_to_body.erase(it);
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
        JPH::RVec3 p = bi.GetCenterOfMassPosition(body_id);
        JPH::Quat q = bi.GetRotation(body_id);
        t.position = Vec3{(float)p.GetX(), (float)p.GetY(), (float)p.GetZ()};
        t.rotation = Quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    }
}

bool PhysicsWorld::has_body(std::uint32_t entity) const {
    if (!impl_) return false;
    return impl_->entity_to_body.find(entity) != impl_->entity_to_body.end();
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

    // MOVING layer, or the new body keeps colliding by static rules.
    JPH::BodyCreationSettings bcs(shape, pos, rot, motion, Layers::MOVING);
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
    bi.MoveKinematic(it->second,
                     JPH::RVec3{pos.x, pos.y, pos.z},
                     JPH::Quat{rot.x, rot.y, rot.z, rot.w},
                     dt);
}

void PhysicsWorld::teleport(std::uint32_t entity, const Vec3& pos,
                             const Quat& rot) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    bi.SetPositionAndRotation(it->second,
                              JPH::RVec3{pos.x, pos.y, pos.z},
                              JPH::Quat{rot.x, rot.y, rot.z, rot.w},
                              JPH::EActivation::Activate);
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
        dt, grounded ? gravity : JPH::Vec3::sZero(), ext,
        impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
        impl_->system.GetDefaultLayerFilter(Layers::MOVING),
        impl_->character_filter, {}, impl_->temp);

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
        shape, 1.5f * impl_->character->GetCharacterPadding(),
        impl_->system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
        impl_->system.GetDefaultLayerFilter(Layers::MOVING),
        impl_->character_filter, {}, impl_->temp);
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
    out_entity = kInvalidEntity;
    if (!impl_) return false;
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f || max_dist <= 0.0f) return false;
    const JPH::Vec3 d{dir.x / len * max_dist, dir.y / len * max_dist,
                      dir.z / len * max_dist};
    JPH::RRayCast ray{JPH::RVec3{origin.x, origin.y, origin.z}, d};
    JPH::RayCastResult hit;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, hit)) return false;

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

}  // namespace ptv
