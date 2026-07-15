#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>

#include "locomotion_settings_menu_shared.h"

namespace ptv {

// Optional explicit persist/load helpers for future save_load_system to call.
extern "C" void ptv_locomotion_force_persist(World* world, std::uint32_t owner_entity) {
    if (!world || !world->valid(owner_entity)) return;
    auto& w = *world;
    if (!w.has<CLocomotionSettings>(owner_entity)) return;
    const auto& s = w.get<CLocomotionSettings>(owner_entity);
    persist_settings_if_present(w, owner_entity, s);
}

extern "C" void ptv_locomotion_try_load(World* world, std::uint32_t owner_entity) {
    if (!world || !world->valid(owner_entity)) return;
    auto& w = *world;
    if (!w.has<CLocomotionSettings>(owner_entity)) return;
    auto& s = w.get<CLocomotionSettings>(owner_entity);
    load_settings_if_present(w, owner_entity, s);
    s.dirty = true; // Re-apply after load.
}

} // namespace ptv
