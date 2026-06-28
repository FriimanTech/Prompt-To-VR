#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// This file provides utility helpers that the register.cpp class uses via ODR linkage.
// Kept minimal: no new registrations here.

namespace ptv {

// Utility to locate player rig entity by a heuristic if needed by other systems.
// Not used directly by the main mechanic (it receives the bound entity), but
// available for future integration tasks.
static std::uint32_t find_player_rig(World& w) {
    // Prefer entity tagged with "player" and "rig" if present
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ent : view) {
        auto e = static_cast<std::uint32_t>(ent);
        if (!w.has<CTag>(e)) continue;
        const auto& tags = w.get<CTag>(e).tags;
        bool has_player = std::find(tags.begin(), tags.end(), std::string("player")) != tags.end();
        bool has_rig    = std::find(tags.begin(), tags.end(), std::string("rig")) != tags.end();
        if (has_player && has_rig) return e;
    }
    return 0u;
}

// Small helper to post a one-time wrist-menu hint as a tag for HUD to consume.
static void post_wrist_hint_if_central(World& w, std::uint32_t e) {
    // Heuristic: if any ambient entity in scene carries a tag like "nav:central_battery_plaza" or name chip we add a hint tag.
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    bool in_central = false;
    for (auto ent : view) {
        auto ee = static_cast<std::uint32_t>(ent);
        if (!w.has<CTag>(ee)) continue;
        const auto& tags = w.get<CTag>(ee).tags;
        for (const auto& t : tags) {
            if (t.find("nav:central_battery_plaza") != std::string::npos) { in_central = true; break; }
        }
        if (in_central) break;
    }
    if (!in_central) return;

    // Attach a toast tag; HUD dependency can translate this into an actual toast.
    CTag tag;
    if (w.has<CTag>(e)) tag = w.get<CTag>(e);
    const std::string key = std::string("toast:") + "hint_open_wrist_menu_locomotion";
    bool present = false;
    for (auto& t : tag.tags) if (t == key) { present = true; break; }
    if (!present) tag.tags.push_back(key);
    w.emplace_or_replace<CTag>(e, std::move(tag));
}

} // namespace ptv
