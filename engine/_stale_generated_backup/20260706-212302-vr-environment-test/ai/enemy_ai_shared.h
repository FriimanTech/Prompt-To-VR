#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <cmath>

namespace ptv {

// Local helper enums/types for this task only
enum class EnemyArchetype : std::uint8_t {
    Unknown = 0,
    Drone,
    Raider,
    Mech,
    Fear
};

struct EnemyTimers {
    float fire_cooldown{0.0f};
    float dodge_cooldown{0.0f};
    float stagger_time{0.0f};
    float behaviour_swap_grace{0.0f};
};

struct EnemyStateLocal {
    EnemyArchetype archetype{EnemyArchetype::Unknown};
    EnemyTimers t{};
    // Movement control
    float orbit_angle{0.0f};
    float desired_altitude{2.5f};
    // Cached perceived player position
    ptv::Vec3 last_player_pos{0.0f, 1.6f, 0.0f};
};

// --------------- Utility helpers (decl) -----------------
std::optional<ptv::Vec3> ptv_ai_find_player(World& w);
bool ptv_ai_has_tag(const ptv::CTag& tag, const std::string& s);
void ptv_ai_add_tag(World& w, std::uint32_t e, const std::string& s);
void ptv_ai_remove_tag(World& w, std::uint32_t e, const std::string& s);
EnemyArchetype ptv_ai_detect_archetype(World& w, std::uint32_t e);

// Movement primitives
ptv::Vec3 ptv_ai_seek(const ptv::Vec3& from, const ptv::Vec3& to, float max_step);
ptv::Vec3 ptv_ai_orbit_point(const ptv::Vec3& center, float radius, float angle_rad, float y);
float      ptv_ai_angle_wrap(float a);

// Per-archetype ticks (declared here, defined in respective .cpp files)
void ptv_ai_tick_drone(World& w, std::uint32_t e, EnemyStateLocal& s, float dt);
void ptv_ai_tick_raider(World& w, std::uint32_t e, EnemyStateLocal& s, float dt);
void ptv_ai_tick_mech(World& w, std::uint32_t e, EnemyStateLocal& s, float dt);
void ptv_ai_tick_fear(World& w, std::uint32_t e, EnemyStateLocal& s, float dt);

// Shared cooldown/timer step
inline void ptv_ai_step_timers(EnemyTimers& t, float dt) {
    t.fire_cooldown = std::max(0.0f, t.fire_cooldown - dt);
    t.dodge_cooldown = std::max(0.0f, t.dodge_cooldown - dt);
    t.stagger_time = std::max(0.0f, t.stagger_time - dt);
    t.behaviour_swap_grace = std::max(0.0f, t.behaviour_swap_grace - dt);
}

// Lightweight perception: in-front cone test from enemy towards player
bool ptv_ai_in_cone(const ptv::Vec3& enemy_pos, const ptv::Vec3& enemy_fwd,
                    const ptv::Vec3& target_pos, float half_angle_deg, float max_range_m);

} // namespace ptv
