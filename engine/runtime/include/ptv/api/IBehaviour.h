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
 * AI / NPC behaviour interface.
 *
 * The engine hosts behaviours exactly like mechanics: a binding whose id was
 * registered with ``PTV_REGISTER_BEHAVIOUR`` is attached, ticked once per
 * frame in the Simulate phase (after physics), and detached with its entity.
 *
 * Override the context-taking forms. ``ctx`` is the same MechanicContext a
 * mechanic receives -- ``ctx.hands->player_entity()`` to find the player,
 * ``ctx.nav`` to route, ``ctx.perception`` to see, ``ctx.combat`` to hurt --
 * and a behaviour moves its NPC by writing ``CLocomotionIntent``, never
 * ``CTransform``. The context-free forms remain so behaviours generated
 * before the context existed keep compiling and running.
 */
class IBehaviour {
public:
    virtual ~IBehaviour() = default;

    /** Called once when bound. PREFERRED FORM; forwards to the legacy one. */
    virtual void on_attach(World& world, std::uint32_t entity,
                           const nlohmann::json& params, MechanicContext& ctx) {
        (void)ctx;
        on_attach(world, entity, params);
    }

    /** Called every frame. PREFERRED FORM; forwards to the legacy one. */
    virtual void on_tick(World& world, std::uint32_t entity, float dt,
                         const InputSnapshot& input, MechanicContext& ctx) {
        (void)input;
        (void)ctx;
        on_tick(world, entity, dt);
    }

    // --- Legacy forms -----------------------------------------------------
    virtual void on_attach(World& world, std::uint32_t entity,
                           const nlohmann::json& params) {
        (void)world;
        (void)entity;
        (void)params;
    }
    virtual void on_tick(World& world, std::uint32_t entity, float dt) {
        (void)world;
        (void)entity;
        (void)dt;
    }

    virtual void on_detach(World& world, std::uint32_t entity) = 0;
    virtual std::string id() const = 0;
};

using BehaviourFactory = std::function<std::unique_ptr<IBehaviour>()>;

bool register_behaviour(const std::string& id, BehaviourFactory factory);

#define PTV_DETAIL_CONCAT_B_(a, b) a##b
#define PTV_DETAIL_CONCAT_B(a, b) PTV_DETAIL_CONCAT_B_(a, b)

#define PTV_REGISTER_BEHAVIOUR(id_literal, TypeName)                                           \
    namespace {                                                                                \
        const bool PTV_DETAIL_CONCAT_B(_ptv_regb_, PTV_DETAIL_CONCAT_B(__LINE__, __COUNTER__)) = \
            ::ptv::register_behaviour(                                                          \
                id_literal,                                                                     \
                [] { return std::unique_ptr<::ptv::IBehaviour>(new TypeName()); });             \
    }

}  // namespace ptv
