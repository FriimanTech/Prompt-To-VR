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
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Geometry/IndexedTriangle.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

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

}  // namespace

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 2};
    BPLayerInterface bp_layers{};
    ObjectVsBPLayerFilter obj_vs_bp{};
    ObjectLayerPairFilter obj_pair{};
    JPH::PhysicsSystem system{};
    std::unordered_map<std::uint32_t, JPH::BodyID> entity_to_body{};
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

    auto view = world.registry().view<CTransform, CPhysics>();
    for (auto entity : view) {
        const auto& t = view.get<CTransform>(entity).t;
        const auto& p = view.get<CPhysics>(entity);

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

        // 1) Inline procedural mesh -> triangle mesh collision (static only;
        //    Jolt mesh shapes cannot be dynamic/kinematic).
        if (motion == JPH::EMotionType::Static &&
            p.collision_shape == "mesh") {
            const CInlineMesh* im = world.registry().try_get<CInlineMesh>(entity);
            if (im != nullptr) {
                shape_ref = build_inline_mesh_shape(*im);
            }
        }

        // 2) Primitive -> non-uniform box / sphere sized by its full extents.
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

        // 3) Fallback: legacy uniform-scale box / sphere (starter room etc.).
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
                     static_cast<std::uint32_t>(entity));
            body_pos = JPH::RVec3::sZero();
        }
        JPH::Quat body_rot{t.rotation.x, t.rotation.y, t.rotation.z,
                           t.rotation.w};
        const float rot_len = body_rot.Length();
        if (!std::isfinite(rot_len) || rot_len < 1.0e-6f) {
            PTV_WARN("Physics: entity {} has degenerate rotation; using identity",
                     static_cast<std::uint32_t>(entity));
            body_rot = JPH::Quat::sIdentity();
        } else if (!body_rot.IsNormalized()) {
            body_rot = body_rot.Normalized();
        }

        JPH::BodyCreationSettings bcs(
            shape_ref,
            body_pos,
            body_rot,
            motion, layer);
        bcs.mRestitution = p.restitution;
        bcs.mFriction = p.friction;
        if (motion != JPH::EMotionType::Static) {
            bcs.mMassPropertiesOverride.mMass = p.mass_kg;
            bcs.mOverrideMassProperties =
                JPH::EOverrideMassProperties::CalculateInertia;
        }
        JPH::BodyID id = bi.CreateAndAddBody(bcs, JPH::EActivation::Activate);
        impl_->entity_to_body[static_cast<std::uint32_t>(entity)] = id;
    }
    impl_->system.OptimizeBroadPhase();
    PTV_INFO("Physics world rebuilt ({} bodies)", impl_->entity_to_body.size());
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

bool PhysicsWorld::raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                           Vec3& out_hit, Vec3& out_normal) const {
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
    return true;
}

}  // namespace ptv
