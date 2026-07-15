#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

#include "aerial_combat_tuning_shared.h"

namespace ptv {

// Utility: search any entity's CTag for one of the provided labels. We treat some labels
// as scene-level hints (e.g., skybox/flight/flying) since runtime scene-id is not exposed.
static bool entity_has_any_tag(const CTag& ctag, const std::vector<std::string>& tags) {
    for (const auto& t : ctag.tags) {
        for (const auto& q : tags) {
            if (t == q) return true;
        }
    }
    return false;
}

bool scene_has_any_tag(World& world, const std::vector<std::string>& tags) {
    auto& reg = world.registry();
    auto view = reg.view<CTag>();
    for (auto e : view) {
        const auto& ctag = view.get<CTag>(e);
        if (entity_has_any_tag(ctag, tags)) return true;
    }
    return false;
}

// Merge helper: set key->value only if different; track if mutated
static bool json_set_if_diff(nlohmann::json& j, const char* key, const nlohmann::json& v) {
    if (!j.contains(key) || j.at(key) != v) { j[key] = v; return true; }
    return false;
}

// Deep set helper for nested objects like ranged.beam.allow_turning
static bool json_set_path(nlohmann::json& j, const std::vector<std::string>& path, const nlohmann::json& v) {
    nlohmann::json* cur = &j;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const auto& k = path[i];
        if (!cur->contains(k) || !(*cur)[k].is_object()) (*cur)[k] = nlohmann::json::object();
        cur = &(*cur)[k];
    }
    const std::string& leaf = path.back();
    if (!cur->contains(leaf) || (*cur)[leaf] != v) { (*cur)[leaf] = v; return true; }
    return false;
}

static bool tweak_flight_binding(CMechanicBinding& bind,
                                const CAerialAssistState& desired,
                                bool enable_open_space) {
    bool mutated = false;
    for (auto& entry : bind.entries) {
        if (entry.id == std::string("generated.flight_mode_mechanic") || entry.builtin_id == std::string("generated.flight_mode_mechanic")) {
            nlohmann::json j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) j = nlohmann::json::object();

            // Ensure buttons/axes are present for acceptance hooks
            mutated |= json_set_if_diff(j, "emergency_brake_button", desired.emergency_brake_button);
            mutated |= json_set_if_diff(j, "boost_button", desired.boost_button);
            mutated |= json_set_if_diff(j, "altitude_axis", desired.altitude_axis);

            // Scene-driven assists
            if (enable_open_space) {
                mutated |= json_set_if_diff(j, "horizon_lock_default", desired.horizon_lock_default);
                mutated |= json_set_if_diff(j, "right_stick_altitude_mode", desired.right_stick_altitude_mode);
                if (!desired.right_stick_altitude_mode) {
                    // Reduce pitch assist gain if altitude mode is off
                    mutated |= json_set_if_diff(j, "pitch_assist_gain", 0.6f);
                } else {
                    // When altitude mode is on, keep pitch assist milder
                    mutated |= json_set_if_diff(j, "pitch_assist_gain", 0.8f);
                }
            } else {
                // Outside open space, relax to defaults
                mutated |= json_set_if_diff(j, "horizon_lock_default", false);
                mutated |= json_set_if_diff(j, "right_stick_altitude_mode", false);
                // Restore neutral gain
                mutated |= json_set_if_diff(j, "pitch_assist_gain", 1.0f);
            }

            // Map B (Right) to quick brake/hover
            mutated |= json_set_if_diff(j, "quick_brake_to_hover_on_button", true);

            if (mutated) {
                entry.params_json = j.dump();
            }
        }
    }
    return mutated;
}

static bool tweak_construct_binding(CMechanicBinding& bind, bool enable_open_space) {
    bool mutated = false;
    for (auto& entry : bind.entries) {
        if (entry.id == std::string("promoted.ring_construct_core") || entry.builtin_id == std::string("promoted.ring_construct_core")) {
            nlohmann::json j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) j = nlohmann::json::object();

            // Publish target engagement cues during sustained beam and turret placement in zero-G
            if (enable_open_space) {
                mutated |= json_set_path(j, {"engagement_cues", "publish"}, true);
                mutated |= json_set_path(j, {"engagement_cues", "context"}, "zero_g");
                // Ensure beam allows turning while sustaining
                mutated |= json_set_path(j, {"ranged", "beam", "allow_turning"}, true);
                // Hint for turret placement reticles in space
                mutated |= json_set_path(j, {"ranged", "turret", "space_place_assist"}, true);
            } else {
                // Keep cues available but not forced
                mutated |= json_set_path(j, {"engagement_cues", "publish"}, false);
            }

            if (mutated) entry.params_json = j.dump();
        }
    }
    return mutated;
}

bool apply_overrides_to_bindings(World& world,
                                 std::uint32_t entity,
                                 const CAerialAssistState& desired,
                                 bool enable_open_space) {
    if (!world.has<CMechanicBinding>(entity)) return false;
    auto& bind = world.get<CMechanicBinding>(entity);
    bool any = false;
    any |= tweak_flight_binding(bind, desired, enable_open_space);
    any |= tweak_construct_binding(bind, enable_open_space);
    if (any) {
        world.emplace_or_replace<CMechanicBinding>(entity, bind);
    }
    return any;
}

} // namespace ptv
