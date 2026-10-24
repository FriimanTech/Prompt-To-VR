#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/api/IBehaviour.h"
#include "ptv/api/IDialogueHook.h"
#include "ptv/api/IMechanic.h"

namespace ptv {

class World;

/** Global registry of mechanic / behaviour / dialogue-hook factories.
 *
 * Built-ins register themselves at static-init time from the .cpp files
 * under engine/runtime/src/mechanics/. Generated code registers via the
 * ``PTV_REGISTER_MECHANIC`` macro. The engine core never links against
 * anything under ``engine/generated/``; instead, we rely on every TU
 * referencing ``register_mechanic`` to live in the same binary.
 */
class MechanicRegistry {
public:
    static MechanicRegistry& get();

    bool register_mechanic(const std::string& id, MechanicFactory f);
    bool register_behaviour(const std::string& id, BehaviourFactory f);
    bool register_dialogue_hook(const std::string& id, DialogueHookFactory f);

    std::unique_ptr<IMechanic> make_mechanic(const std::string& id) const;
    std::unique_ptr<IBehaviour> make_behaviour(const std::string& id) const;
    std::unique_ptr<IDialogueHook> make_dialogue_hook(const std::string& id) const;

    std::vector<std::string> mechanic_ids() const;

private:
    std::unordered_map<std::string, MechanicFactory> mechanics_;
    std::unordered_map<std::string, BehaviourFactory> behaviours_;
    std::unordered_map<std::string, DialogueHookFactory> dialogues_;
};

/**
 * Instantiate and attach all bindings found on an entity's
 * ``CMechanicBinding`` components. Safe to call more than once.
 */
struct MechanicInstance {
    std::string id;
    std::unique_ptr<IMechanic> impl;
    /** Cached from ``impl->phase()`` so the per-frame dispatch does not pay a
     *  virtual call for every instance in every phase. */
    MechanicPhase phase{MechanicPhase::Simulate};
    /** Every entity this instance is attached to. Usually exactly one; a
     *  ``MechanicScope::Player`` instance collects all of its bound entities
     *  here so ``on_detach`` can be delivered for each. */
    std::vector<std::uint32_t> entities;
};

class MechanicHost {
public:
    void attach_bindings(World& world, MechanicContext& ctx);
    /** Tick every instance whose ``phase()`` matches ``phase``.
     *
     *  The frame runs ``Input`` before the InputSnapshot is frozen and before
     *  the physics step, then ``Simulate`` after physics. See MechanicPhase. */
    void update(World& world, float dt, const InputSnapshot& input,
                MechanicContext& ctx, MechanicPhase phase);
    void detach_all(World& world);

    /** Attach the bindings of ONE entity that appeared after the scene load.
     *
     *  ``attach_bindings`` runs once at startup, so an entity spawned during
     *  play had a mesh and a body but none of its mechanics ever ticked --
     *  a spawned enemy just stood there. Safe to call on an entity with no
     *  ``CMechanicBinding``; it simply does nothing. */
    void attach_entity(World& world, std::uint32_t entity,
                       MechanicContext& ctx);

    /** Detach and destroy everything attached to one entity. Shared
     *  player-scoped instances survive; the entity is just dropped from
     *  their attachment list. */
    void detach_entity(World& world, std::uint32_t entity);

private:
    /** Attach one entity's bindings without touching ``player_scoped_``
     *  bookkeeping beyond adding to it. */
    void attach_one(World& world, std::uint32_t entity, MechanicContext& ctx);
    /** Recompute ``player_scoped_`` after ``instances_`` is reordered. */
    void reindex_player_scoped();
    /** Stable-sort instances by ``IMechanic::order`` and reindex. */
    void sort_by_order();

    std::vector<std::pair<std::uint32_t, MechanicInstance>> instances_;
    /** Player-scoped mechanic id -> index into ``instances_``. Persisted
     *  across calls so an entity spawned mid-game joins the existing shared
     *  instance instead of starting a second copy of, say, grab. */
    std::unordered_map<std::string, std::size_t> player_scoped_;
};

}  // namespace ptv
