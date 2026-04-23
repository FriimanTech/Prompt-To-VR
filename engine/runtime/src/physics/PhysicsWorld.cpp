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
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>

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
        JPH::Shape* shape = nullptr;
        if (p.collision_shape == "sphere") {
            shape = new JPH::SphereShape(0.5f * t.scale);
        } else {
            JPH::Vec3 half{0.25f * t.scale, 0.25f * t.scale, 0.25f * t.scale};
            shape = new JPH::BoxShape(half);
        }
        JPH::ShapeRefC shape_ref = shape;
        JPH::EMotionType motion = JPH::EMotionType::Static;
        JPH::ObjectLayer layer = Layers::STATIC;
        if (p.kind == "dynamic") {
            motion = JPH::EMotionType::Dynamic;
            layer = Layers::MOVING;
        } else if (p.kind == "kinematic") {
            motion = JPH::EMotionType::Kinematic;
            layer = Layers::MOVING;
        }
        JPH::BodyCreationSettings bcs(
            shape_ref,
            JPH::RVec3{t.position.x, t.position.y, t.position.z},
            JPH::Quat{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w},
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

}  // namespace ptv
