#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "ptv/api/InputSnapshot.h"
#include "ptv/api/MechanicContext.h"

namespace ptv {

class World;

/**
 * When in the frame a mechanic runs.
 *
 * Ordering matters because some mechanics PRODUCE state that others CONSUME
 * in the same frame. Grab is the canonical case: which entity a hand is
 * holding must be decided before the immutable ``InputSnapshot`` is frozen,
 * otherwise every other mechanic sees last frame's ``held_entity`` and a
 * weapon mechanic would not notice it was picked up until the next frame.
 *
 *   Input    -- runs BEFORE the snapshot is frozen and BEFORE the physics
 *               step. For mechanics that move the player or decide what is
 *               held. Their effects are visible to everyone else this frame.
 *   Simulate -- the default. Runs AFTER physics with the frozen snapshot, so
 *               ECS transforms are already settled for this frame.
 *
 * Generated mechanics almost always want the default.
 */
enum class MechanicPhase : int {
    Input = 0,
    Simulate = 1,
};

/**
 * How many instances of a mechanic exist.
 *
 *   Entity -- the default: one instance per bound entity. Bind it to ten
 *             crates and ten instances tick, each minding its own crate.
 *   Player -- a single shared instance no matter how many entities bind it.
 *             ``on_attach`` still runs for EVERY bound entity (so the
 *             mechanic can mark each one, e.g. "this crate is grabbable"),
 *             but ``on_update`` ticks exactly ONCE per frame.
 *
 * Player scope exists because player-global systems would otherwise
 * multiply: ``builtin.grab`` is bound to every grabbable prop, and a global
 * per-hand grab loop running once per prop would grab and throw N times.
 */
enum class MechanicScope : int {
    Entity = 0,
    Player = 1,
};

/**
 * Stable interface that every generated or built-in mechanic implements.
 *
 * Generated C++ modules live under engine/generated/mechanics/ and must
 * implement this interface. The engine core never imports generated headers.
 */
class IMechanic {
public:
    virtual ~IMechanic() = default;

    /** Called once when the mechanic is bound to an entity. PREFERRED FORM.
     *
     * For ``MechanicScope::Player`` mechanics this is called once PER BOUND
     * ENTITY on the same shared instance, which is how e.g. grab marks every
     * bound prop as grabbable while still ticking only once.
     *
     * The default forwards to the legacy context-free overload below, so
     * mechanics written before ``MechanicContext`` existed keep working. */
    virtual void on_attach(World& world, std::uint32_t entity,
                           const nlohmann::json& params,
                           MechanicContext& ctx) {
        (void)ctx;
        on_attach(world, entity, params);
    }

    /** Called every frame. PREFERRED FORM.
     *
     * ``input`` is a read-only snapshot of live controller + headset input
     * for this frame (buttons, triggers, grips, thumbsticks, hand/aim/head
     * poses in world space, and which entity each hand is holding). Poses are
     * already rig-composed, so they are directly comparable with entity
     * ``CTransform`` positions. See ptv/api/InputSnapshot.h.
     *
     * ``ctx`` exposes the engine services a mechanic is allowed to touch:
     * locomotion (move the player), physics (raycast / impulse / kinematic),
     * and one-frame debug drawing. See ptv/api/MechanicContext.h.
     *
     * The default forwards to the legacy context-free overload below. */
    virtual void on_update(World& world, std::uint32_t entity, float dt,
                           const InputSnapshot& input, MechanicContext& ctx) {
        (void)ctx;
        on_update(world, entity, dt, input);
    }

    // --- Legacy overloads --------------------------------------------------
    // Mechanics generated before MechanicContext existed override these. New
    // code should override the context-taking forms above instead; these stay
    // so previously generated projects keep compiling unchanged.

    virtual void on_attach(World& world, std::uint32_t entity,
                           const nlohmann::json& params) {
        (void)world;
        (void)entity;
        (void)params;
    }

    virtual void on_update(World& world, std::uint32_t entity, float dt,
                           const InputSnapshot& input) {
        (void)world;
        (void)entity;
        (void)dt;
        (void)input;
    }

    /** Called when the mechanic is detached or the scene unloads. */
    virtual void on_detach(World& world, std::uint32_t entity) = 0;

    /** Stable identifier, e.g. "builtin.grab" or "generated.repulsor_shooter". */
    virtual std::string id() const = 0;

    /** When in the frame this mechanic runs. Defaults to ``Simulate``. */
    virtual MechanicPhase phase() const { return MechanicPhase::Simulate; }

    /** How many instances exist. Defaults to one per bound entity. */
    virtual MechanicScope scope() const { return MechanicScope::Entity; }
};

using MechanicFactory = std::function<std::unique_ptr<IMechanic>()>;

/**
 * Register a mechanic factory at startup.
 *
 * Defined in engine/runtime/src/mechanics/MechanicRegistry.cpp. Generated code
 * calls ::ptv::register_mechanic() via the REGISTER_MECHANIC macro below.
 */
bool register_mechanic(const std::string& id, MechanicFactory factory);

#define PTV_DETAIL_CONCAT_(a, b) a##b
#define PTV_DETAIL_CONCAT(a, b) PTV_DETAIL_CONCAT_(a, b)

/**
 * Register a mechanic factory at file scope. Safe to use with either a
 * plain, unqualified TypeName (``RingHoverFlight``) or a fully-qualified
 * one (``ptv::RingHoverFlight``, ``mygame::Foo``, etc.). The registration
 * variable is uniquified by ``__LINE__`` + ``__COUNTER__`` so it never
 * collides and never token-pastes ``::`` into an identifier.
 *
 * Must be invoked at namespace scope (NOT inside a class/function body).
 */
#define PTV_REGISTER_MECHANIC(id_literal, TypeName)                                          \
    namespace {                                                                              \
        const bool PTV_DETAIL_CONCAT(_ptv_reg_, PTV_DETAIL_CONCAT(__LINE__, __COUNTER__)) =  \
            ::ptv::register_mechanic(                                                        \
                id_literal,                                                                  \
                [] { return std::unique_ptr<::ptv::IMechanic>(new TypeName()); });           \
    }

}  // namespace ptv
