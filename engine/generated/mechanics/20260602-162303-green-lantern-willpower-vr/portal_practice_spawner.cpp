#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

namespace ptv {

// These are declared as members in PortalTravel, but we provide thin free-function
// wrappers so other mechanics (tests) could reuse the tag/binding pattern if needed.
namespace portal_practice_internal {

static void ensure_entry(World& w, std::uint32_t e, int max_alive) {
    CMechanicBinding mb;
    if (w.has<CMechanicBinding>(e)) mb = w.get<CMechanicBinding>(e);

    bool found = false;
    for (auto& en : mb.entries) {
        if (en.id == "practice_drones" && en.builtin_id == "builtin.spawner") { found = true; break; }
    }
    if (!found) {
        CMechanicBinding::Entry en{};
        en.id = "practice_drones";
        en.builtin_id = "builtin.spawner";
        nlohmann::json pj{{"spawn_kind","drone_practice"},{"max_alive", std::max(0, max_alive)},{"rate_per_s",0.5}};
        en.params_json = pj.dump();
        mb.entries.push_back(std::move(en));
        w.emplace_or_replace<CMechanicBinding>(e, std::move(mb));
    }
}

static void remove_entry(World& w, std::uint32_t e) {
    if (!w.has<CMechanicBinding>(e)) return;
    auto mb = w.get<CMechanicBinding>(e);
    auto& v = mb.entries;
    v.erase(std::remove_if(v.begin(), v.end(), [](const CMechanicBinding::Entry& en){
        return en.id == "practice_drones" && en.builtin_id == "builtin.spawner"; }), v.end());
    w.emplace_or_replace<CMechanicBinding>(e, std::move(mb));
}

} // namespace portal_practice_internal

} // namespace ptv
