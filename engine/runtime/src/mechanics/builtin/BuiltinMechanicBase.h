#pragma once

// Shared CRTP base for the engine's built-in mechanics.
//
// Each built-in lives in its own module directory under
// ``engine/runtime/src/mechanics/builtin/<mechanic>/`` and may span several
// files. They are ordinary IMechanic implementations -- the same interface
// generated code implements -- so a ``builtin+config_code_edit`` task can copy
// one of these modules and edit it into a new generated mechanic.

#include <string>

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

namespace ptv {

/** Cuts the boilerplate every built-in would otherwise repeat.
 *
 * Derived provides:
 *   static constexpr const char* kId = "builtin.something";
 *   void configure(const nlohmann::json& params);
 *   void tick(World&, std::uint32_t entity, float dt,
 *             const InputSnapshot&, MechanicContext&);
 *
 * Override ``on_attach`` directly instead of ``configure`` when the mechanic
 * needs the world/entity at attach time (e.g. to mark an entity grabbable).
 * Override ``phase()`` / ``scope()`` to opt out of the defaults.
 */
template <typename Derived>
class BuiltinMechanicBase : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json& p,
                   MechanicContext&) override {
        static_cast<Derived*>(this)->configure(p);
    }
    void on_update(World& world, std::uint32_t entity, float dt,
                   const InputSnapshot& input, MechanicContext& ctx) override {
        static_cast<Derived*>(this)->tick(world, entity, dt, input, ctx);
    }
    void on_detach(World&, std::uint32_t) override {}
    std::string id() const override { return Derived::kId; }
};

}  // namespace ptv
