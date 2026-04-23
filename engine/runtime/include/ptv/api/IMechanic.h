#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

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

    /** Called every frame after input and physics. */
    virtual void on_update(World& world, std::uint32_t entity, float dt) = 0;

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

#define PTV_REGISTER_MECHANIC(id_literal, TypeName)                                          \
    namespace {                                                                              \
        const bool _ptv_reg_##TypeName = ::ptv::register_mechanic(                           \
            id_literal, [] { return std::unique_ptr<::ptv::IMechanic>(new TypeName()); });   \
    }

}  // namespace ptv
