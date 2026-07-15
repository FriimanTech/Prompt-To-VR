#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <algorithm>

#include "civilian_rescue_shared.h"

namespace ptv {

// Optional visual side-effects helper: thicken scale slightly on civilians with active shields.
// This is a pure cosmetic nudge until a proper VFX system hooks in.
static void apply_shield_visual(World& w, std::uint32_t civ, const ShieldData& sd) {
    if (!w.has<CTransform>(civ)) return;
    auto& tr = w.get<CTransform>(civ).t;
    if (sd.active) {
        float alpha = std::clamp(sd.hp / std::max(1.0f, sd.max_hp), 0.0f, 1.0f);
        tr.scale = 1.0f + 0.05f * alpha; // Transform::scale is a single float
    } else {
        tr.scale = 1.0f;
    }
}

// Drive light VFX every frame by scanning controllers that run this mechanic and peeking at shield map via tags.
// Since we cannot share mechanic internals here, we infer by presence of a temporary "shielded" tag the gameplay may add later.
// For now, keep this file as a placeholder for visual thickening when mission authors decide to tag civilians.
void ptv_rescue_vfx_tick(World& w, float /*dt*/) {
    auto& reg = w.registry();
    auto v = reg.view<CTag, CTransform>();
    for (auto e : v) {
        const auto& tg = v.get<CTag>(e);
        bool is_civ = tag_list_contains(tg.tags, "civilian") || tag_list_contains(tg.tags, "npc");
        bool tagged_shield = tag_list_contains(tg.tags, "shielded");
        if (!is_civ) continue;
        ShieldData sd;
        sd.active = tagged_shield; sd.max_hp = 100.0f; sd.hp = tagged_shield ? 100.0f : 0.0f;
        apply_shield_visual(w, static_cast<std::uint32_t>(e), sd);
    }
}

} // namespace ptv
