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

#include <algorithm>
#include <cmath>
#include <cstdint>
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
JPH::ShapeRefC build_capsule_shape(float height_m) {
    const float height = std::max(0.20f, height_m);
    const float radius = std::clamp(0.17f * height, 0.05f, 0.45f * height);
    const float half_cylinder = std::max(0.02f, 0.5f * height - radius);
    return new JPH::CapsuleShape(half_cylinder, radius);
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
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() { shutdown(); }

bool PhysicsWorld::initialize() {
    if (impl_) return true;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = nullptr;
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
        bcs.mMassPropertiesOverride.mMass = p.mass_kg;
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
    impl_->system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sOne(), com_transform, settings,
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

void PhysicsWorld::set_kinematic(std::uint32_t entity, bool kinematic) {
    if (!impl_) return;
    auto it = impl_->entity_to_body.find(entity);
    if (it == impl_->entity_to_body.end()) return;
    auto& bi = impl_->system.GetBodyInterface();
    const JPH::EMotionType target = kinematic ? JPH::EMotionType::Kinematic
                                              : JPH::EMotionType::Dynamic;
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
    if (!impl_) return delta;
    const float want = glm::length(delta);
    if (want < 1.0e-6f) return delta;

    JPH::ShapeRefC shape = build_capsule_shape(height);
    if (shape.GetPtr() == nullptr) return delta;

    // Exclude the mover's own body, otherwise the very first cast reports a
    // zero-distance hit against itself and the player can never move.
    JPH::BodyID self;
    if (auto it = impl_->entity_to_body.find(ignore_entity);
        it != impl_->entity_to_body.end()) {
        self = it->second;
    }
    JPH::IgnoreSingleBodyFilter body_filter(self);

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
        const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
            shape, JPH::Vec3::sOne(), start,
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

        const Vec3 leftover = remaining * (1.0f - safe);
        remaining = leftover - n * glm::dot(leftover, n);
    }

    // Fail open. If we resolved to almost nothing the mover is probably
    // wedged inside geometry, and in VR being frozen in place is a worse
    // failure than clipping through a wall for one frame.
    if (glm::length(moved) < 0.02f * want) return delta;
    return moved;
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
