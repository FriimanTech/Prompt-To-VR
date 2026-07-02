#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <cmath>

namespace ptv {

struct ParsedControlInput {
    // Dynamic, per-frame inputs driven by the binding params_json.
    bool left_trigger{false};
    bool right_trigger{false};
    bool left_grip{false};

    // Selection from construct core: which control is currently armed.
    // Expected: "chain_tether" or "restraining_bands".
    std::string active_control{"chain_tether"};

    // Optional refined target id (string registered in world).
    std::string refined_target_id{};

    // Config params (fall back to defaults when not provided).
    float tether_max_length_m{18.0f};
    float tether_tension_break_threshold{1.0f}; // normalized
    float reel_in_speed_mps{2.0f};
    float bands_slow_per_stack{0.18f};
    int   bands_max_stacks{3};
    float fear_zone_resistance_multiplier{1.5f};
};

struct BandsRuntime {
    int stacks{0};
    float time_since_last_apply_s{0.0f};
};

struct TetherRuntime {
    bool active{false};
    std::uint32_t target_entity{0};
    ptv::Vec3 anchor_ws{0.0f,0.0f,0.0f}; // player hand/world point at latch
    float tension_norm{0.0f};
};

// Access helpers
ParsedControlInput parse_control_binding_params(World& w, std::uint32_t binding_entity, const std::string& self_mechanic_id);

// Utility: safe get entity by string id (returns entt::null as 0 if not found)
std::uint32_t find_entity(World& w, const std::string& id_str);

// Utility: push a tag string into CTag.tags if not already present
void ensure_tag(World& w, std::uint32_t e, const std::string& tag);
// Utility: remove any tag that starts with prefix
void remove_tag_prefix(World& w, std::uint32_t e, const std::string& prefix);
// Utility: check if an entity has a plain tag equal to key OR any tag that starts with key+
bool has_tag_or_prefix(World& w, std::uint32_t e, const std::string& key);

// Haptics publish helper (uses dependency contract)
void publish_haptics(World& w, std::uint32_t e, const std::string& pattern_id,
                     const std::string& hand, float intensity_scale, float duration_s);

// VFX publish helper (generic tag-based)
void spawn_vfx_line(World& w, std::uint32_t e_context,
                    const ptv::Vec3& from, const ptv::Vec3& to,
                    const std::string& asset_id,
                    float ttl_s);

// Cage tighten hint
void request_cage_tighten(World& w, std::uint32_t target,
                          float delta_radius_m, float duration_add_s, float drain_mul);

// Yellow zone detection on target (via tags)
bool is_in_yellow_zone(World& w, std::uint32_t target);

// Chain/Tether core step. Returns whether tether remains active.
bool step_tether(World& w, std::uint32_t player_hand_entity, float dt,
                 const ParsedControlInput& in,
                 TetherRuntime& tr);

// Restraining bands application and decay
void apply_bands_now(World& w, std::uint32_t target, const ParsedControlInput& in,
                     BandsRuntime& br);
void decay_bands(World& w, std::uint32_t target, float dt,
                 const ParsedControlInput& in, BandsRuntime& br);

} // namespace ptv
