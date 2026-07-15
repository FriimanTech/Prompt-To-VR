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
 * Built-ins register themselves at static-init time from
 * engine/runtime/src/mechanics/*.cpp. Generated code registers via the
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
};

class MechanicHost {
public:
    void attach_bindings(World& world);
    void update(World& world, float dt, const InputSnapshot& input);
    void detach_all(World& world);

private:
    std::vector<std::pair<std::uint32_t, MechanicInstance>> instances_;
};

}  // namespace ptv
