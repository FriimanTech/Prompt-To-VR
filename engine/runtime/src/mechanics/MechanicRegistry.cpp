#include "ptv/mechanics/MechanicRegistry.h"

#include <algorithm>
#include <cstddef>
#include <unordered_map>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"

namespace ptv {

// See BuiltinMechanics.cpp for the explanation. Referencing this symbol
// anywhere in a TU that *is* linked guarantees MSVC pulls BuiltinMechanics.obj
// into the final binary, which in turn runs every PTV_REGISTER_MECHANIC.
extern "C" int ptv_force_link_builtin_mechanics;
static const int _ptv_anchor_builtin_mechanics = ptv_force_link_builtin_mechanics;

MechanicRegistry& MechanicRegistry::get() {
    static MechanicRegistry r;
    // Silence -Wunused-variable and ensure the anchor is actually referenced
    // at runtime (belt-and-braces; the static-init above is sufficient).
    (void)_ptv_anchor_builtin_mechanics;
    return r;
}

bool MechanicRegistry::register_mechanic(const std::string& id, MechanicFactory f) {
    mechanics_[id] = std::move(f);
    return true;
}
bool MechanicRegistry::register_behaviour(const std::string& id, BehaviourFactory f) {
    behaviours_[id] = std::move(f);
    return true;
}
bool MechanicRegistry::register_dialogue_hook(const std::string& id, DialogueHookFactory f) {
    dialogues_[id] = std::move(f);
    return true;
}

std::unique_ptr<IMechanic> MechanicRegistry::make_mechanic(const std::string& id) const {
    auto it = mechanics_.find(id);
    if (it == mechanics_.end()) return nullptr;
    return it->second();
}
std::unique_ptr<IBehaviour> MechanicRegistry::make_behaviour(const std::string& id) const {
    auto it = behaviours_.find(id);
    if (it == behaviours_.end()) return nullptr;
    return it->second();
}
std::unique_ptr<IDialogueHook> MechanicRegistry::make_dialogue_hook(const std::string& id) const {
    auto it = dialogues_.find(id);
    if (it == dialogues_.end()) return nullptr;
    return it->second();
}

std::vector<std::string> MechanicRegistry::mechanic_ids() const {
    std::vector<std::string> out;
    out.reserve(mechanics_.size());
    for (auto& [k, _] : mechanics_) out.push_back(k);
    return out;
}

bool register_mechanic(const std::string& id, MechanicFactory f) {
    return MechanicRegistry::get().register_mechanic(id, std::move(f));
}
bool register_behaviour(const std::string& id, BehaviourFactory f) {
    return MechanicRegistry::get().register_behaviour(id, std::move(f));
}
bool register_dialogue_hook(const std::string& id, DialogueHookFactory f) {
    return MechanicRegistry::get().register_dialogue_hook(id, std::move(f));
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

void MechanicHost::attach_bindings(World& world, MechanicContext& ctx) {
    detach_all(world);

    auto view = world.registry().view<CMechanicBinding>();
    for (auto entity : view) {
        attach_one(world, static_cast<std::uint32_t>(entity), ctx);
    }
    PTV_INFO("Attached {} mechanic instances", instances_.size());
}

void MechanicHost::attach_one(World& world, std::uint32_t entity,
                              MechanicContext& ctx) {
    const auto e = static_cast<entt::entity>(entity);
    if (!world.registry().valid(e)) return;
    const auto* bindings = world.registry().try_get<CMechanicBinding>(e);
    if (bindings == nullptr) return;

    for (const auto& entry : bindings->entries) {
        std::string resolved =
            entry.builtin_id.empty() ? entry.id : entry.builtin_id;

        nlohmann::json params = nlohmann::json::object();
        if (!entry.params_json.empty()) {
            try {
                params = nlohmann::json::parse(entry.params_json);
            } catch (...) { /* ignore malformed params */ }
        }

        // Player-scoped mechanics get ONE instance no matter how many
        // entities bind them, so later bindings of the same id attach their
        // entity to the existing instance instead of adding another ticking
        // copy (otherwise grab, bound to every grabbable prop, would run its
        // per-hand loop once per prop).
        if (auto found = player_scoped_.find(resolved);
            found != player_scoped_.end()) {
            auto& existing = instances_[found->second].second;
            existing.impl->on_attach(world, entity, params, ctx);
            existing.entities.push_back(entity);
            continue;
        }

        auto impl = MechanicRegistry::get().make_mechanic(resolved);
        if (!impl) {
            PTV_WARN("No mechanic registered for id: {}", resolved);
            continue;
        }
        const bool is_player_scoped = impl->scope() == MechanicScope::Player;
        const MechanicPhase phase = impl->phase();

        impl->on_attach(world, entity, params, ctx);
        // emplace_back with explicit MechanicInstance so MSVC can move
        // the unique_ptr through the pair constructor (brace-init of a
        // pair containing a move-only member is rejected by MSVC).
        MechanicInstance inst{};
        inst.id = resolved;
        inst.impl = std::move(impl);
        inst.phase = phase;
        inst.entities.push_back(entity);
        instances_.emplace_back(entity, std::move(inst));
        if (is_player_scoped) {
            player_scoped_.emplace(resolved, instances_.size() - 1);
        }
    }
}

void MechanicHost::attach_entity(World& world, std::uint32_t entity,
                                 MechanicContext& ctx) {
    attach_one(world, entity, ctx);
}

void MechanicHost::detach_entity(World& world, std::uint32_t entity) {
    for (auto it = instances_.begin(); it != instances_.end();) {
        auto& inst = it->second;
        if (it->first == entity) {
            if (inst.impl) {
                for (std::uint32_t attached : inst.entities) {
                    inst.impl->on_detach(world, attached);
                }
            }
            it = instances_.erase(it);
            continue;
        }
        // A shared player-scoped instance owned by some other entity: drop
        // just this entity from it, keeping the instance alive for the rest.
        auto& list = inst.entities;
        auto found = std::find(list.begin(), list.end(), entity);
        if (found != list.end()) {
            if (inst.impl) inst.impl->on_detach(world, entity);
            list.erase(found);
        }
        ++it;
    }
    reindex_player_scoped();
}

void MechanicHost::reindex_player_scoped() {
    player_scoped_.clear();
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        auto& inst = instances_[i].second;
        if (!inst.impl) continue;
        if (inst.impl->scope() != MechanicScope::Player) continue;
        player_scoped_.emplace(inst.id, i);
    }
}

void MechanicHost::update(World& world, float dt, const InputSnapshot& input,
                          MechanicContext& ctx, MechanicPhase phase) {
    for (auto& [eid, inst] : instances_) {
        if (inst.impl && inst.phase == phase) {
            inst.impl->on_update(world, eid, dt, input, ctx);
        }
    }
}

void MechanicHost::detach_all(World& world) {
    for (auto& [eid, inst] : instances_) {
        if (!inst.impl) continue;
        // Player-scoped instances collected every entity they were attached
        // to, so each gets a matching on_detach.
        for (std::uint32_t attached : inst.entities) {
            inst.impl->on_detach(world, attached);
        }
    }
    instances_.clear();
    player_scoped_.clear();
}

}  // namespace ptv
