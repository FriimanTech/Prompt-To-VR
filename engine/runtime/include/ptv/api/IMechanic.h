#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "ptv/api/InputSnapshot.h"

namespace ptv {

class World;

/**
 * Stable interface that every generated or built-in mechanic implements.
 *
 * Generated C++ modules live under engine/generated/mechanics/ and must
 * implement this interface. The engine core never imports generated headers.
 */
class IMechanic {
public:
    virtual ~IMechanic() = default;

    /** Called once when the mechanic is bound to an entity. */
    virtual void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) = 0;

    /** Called every frame after input and physics.
     *
     * ``input`` is a read-only snapshot of live controller + headset input
     * for this frame (buttons, triggers, grips, thumbsticks, hand/aim/head
     * poses in world space, and which entity each hand is holding). Poses are
     * already rig-composed, so they are directly comparable with entity
     * ``CTransform`` positions. See ptv/api/InputSnapshot.h. */
    virtual void on_update(World& world, std::uint32_t entity, float dt,
                           const InputSnapshot& input) = 0;

    /** Called when the mechanic is detached or the scene unloads. */
    virtual void on_detach(World& world, std::uint32_t entity) = 0;

    /** Stable identifier, e.g. "builtin.grab" or "generated.repulsor_shooter". */
    virtual std::string id() const = 0;
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
