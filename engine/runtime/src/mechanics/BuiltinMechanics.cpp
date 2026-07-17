// Built-in reusable VR mechanics.
//
// These are intentionally small, data-driven, and stateless across frames
// except for per-entity state that lives on ECS components. Generated C++
// mechanics extend the palette under engine/generated/ but never replace the
// behaviour implemented here.

#include "ptv/api/IMechanic.h"
#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

namespace ptv {

// Anchor symbol: MSVC's linker drops an entire .obj from a static library if
// nothing references it. Our built-in mechanics register themselves via
// static constructors, so without a reference the registrations never run.
// MechanicRegistry.cpp references ``ptv_force_link_builtin_mechanics`` to
// force this translation unit (and every PTV_REGISTER_MECHANIC below) to be
// linked in.
extern "C" int ptv_force_link_builtin_mechanics = 1;

// ---------------------------------------------------------------------------
// Tiny base class to cut boilerplate.
// ---------------------------------------------------------------------------
template <typename Derived>
class BuiltinMechanicBase : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json& p) override {
        static_cast<Derived*>(this)->configure(p);
    }
    void on_update(World& world, std::uint32_t entity, float dt,
                   const InputSnapshot& input) override {
        static_cast<Derived*>(this)->tick(world, entity, dt, input);
    }
    void on_detach(World&, std::uint32_t) override {}
    std::string id() const override { return Derived::kId; }
    // Derived provides:
    //   static constexpr const char* kId = "...";
    //   void configure(const nlohmann::json& params);
    //   void tick(World&, std::uint32_t, float, const InputSnapshot&);
};

// ---------------------------------------------------------------------------
// Components owned by specific built-ins (kept trivial).
// ---------------------------------------------------------------------------
struct CGrabbable { bool held{false}; };

// ---------------------------------------------------------------------------
// Teleport
// ---------------------------------------------------------------------------
class TeleportMechanic : public BuiltinMechanicBase<TeleportMechanic> {
public:
    static constexpr const char* kId = "builtin.teleport";
    void configure(const nlohmann::json&) {}
    void tick(World&, std::uint32_t, float, const InputSnapshot&) {}
};
PTV_REGISTER_MECHANIC("builtin.teleport", TeleportMechanic)
// Planner / engine-catalog ids (locomotion is implemented in Application.cpp;
// these registrations exist so ``MechanicHost`` can attach scene bindings
// without logging "No mechanic registered" and so generated ``scene.json``
// stays aligned with ``docs/locomotion.md``).
PTV_REGISTER_MECHANIC("builtin.locomotion.teleport", TeleportMechanic)

// ---------------------------------------------------------------------------
// Smooth (sliding) locomotion -- catalog stub.
//
// IMPORTANT: ``tick`` here is INTENTIONALLY empty. The real Head-Relative /
// Controller-Relative slide logic lives in ``Application.cpp`` (search for
// ``LocomotionMode::SlideHeadRelative`` / ``SlideControllerRelative``),
// where it has direct access to the per-frame OpenXR pose, the thumbstick
// input, and the player rig transform. We only register these mechanic
// IDs so that every game's ``scene.json`` -- starter room, generated
// Green-Lantern, any future project -- can attach them on the
// ``systems_global`` entity without ``MechanicHost`` logging
// "no mechanic registered for ...".
//
// Do NOT add movement code here -- doing so would double-apply input
// (Application.cpp already moves the rig) and would leave the Head-Relative
// vs Controller-Relative branching out of sync with the rest of the engine.
// ---------------------------------------------------------------------------
class SmoothLocomotionMechanic : public BuiltinMechanicBase<SmoothLocomotionMechanic> {
public:
    static constexpr const char* kId = "builtin.smooth_locomotion";
    void configure(const nlohmann::json& p) {
        speed_ = p.value("speed_mps", 1.5f);
    }
    void tick(World&, std::uint32_t, float, const InputSnapshot&) {}
private:
    float speed_{1.5f};
};
PTV_REGISTER_MECHANIC("builtin.smooth_locomotion", SmoothLocomotionMechanic)
// Catalog-aligned aliases. The actual Head-Relative slide reads the latest
// HMD pose every frame in Application.cpp via XR_REFERENCE_SPACE_TYPE_VIEW
// located against the play-space stage; the actual Controller-Relative
// slide reads the latest left grip pose. Both are pose-relative, never
// world-fixed -- see docs/locomotion.md.
PTV_REGISTER_MECHANIC("builtin.locomotion.slide.head_relative",
                      SmoothLocomotionMechanic)
PTV_REGISTER_MECHANIC("builtin.locomotion.slide.controller_relative",
                      SmoothLocomotionMechanic)

// ---------------------------------------------------------------------------
// Snap turn
// ---------------------------------------------------------------------------
class SnapTurnMechanic : public BuiltinMechanicBase<SnapTurnMechanic> {
public:
    static constexpr const char* kId = "builtin.snap_turn";
    void configure(const nlohmann::json& p) {
        degrees_ = p.value("degrees", 30.0f);
    }
    void tick(World&, std::uint32_t, float, const InputSnapshot&) {}
private:
    float degrees_{30.0f};
};
PTV_REGISTER_MECHANIC("builtin.snap_turn", SnapTurnMechanic)
PTV_REGISTER_MECHANIC("builtin.locomotion.snap_turn", SnapTurnMechanic)

// ---------------------------------------------------------------------------
// Grab
// ---------------------------------------------------------------------------
class GrabMechanic : public BuiltinMechanicBase<GrabMechanic> {
public:
    static constexpr const char* kId = "builtin.grab";
    void configure(const nlohmann::json&) {}
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json&) override {
        world.registry().emplace_or_replace<CGrabbable>(
            static_cast<entt::entity>(entity), CGrabbable{});
    }
    void tick(World&, std::uint32_t, float, const InputSnapshot&) {}
};
PTV_REGISTER_MECHANIC("builtin.grab", GrabMechanic)

// ---------------------------------------------------------------------------
// Throw
// ---------------------------------------------------------------------------
class ThrowMechanic : public BuiltinMechanicBase<ThrowMechanic> {
public:
    static constexpr const char* kId = "builtin.throw";
    void configure(const nlohmann::json& p) {
        impulse_ = p.value("impulse", 4.0f);
    }
    void tick(World&, std::uint32_t, float, const InputSnapshot&) {}
private:
    float impulse_{4.0f};
};
PTV_REGISTER_MECHANIC("builtin.throw", ThrowMechanic)

}  // namespace ptv
