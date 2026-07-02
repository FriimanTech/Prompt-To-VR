#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace behemoth {

enum class Phase : std::uint8_t { P1=0, P2=1, P3=2, Dead=3 };

struct ArmorPlate {
    float max_counter{100.0f};
    float counter{100.0f};
    bool open{false};
    bool broken{false};
};

struct AttackTimers {
    // Beam sweep (P1 primarily)
    float beam_cooldown_s{4.0f};
    float beam_timer_s{0.0f};
    bool  beam_active{false};
    float beam_active_s{0.0f};
    float beam_active_duration_s{2.0f};

    // Corruption surge (P3)
    float surge_period_s{8.0f};
    float surge_timer_s{0.0f};
    bool  surge_active{false};
    float surge_active_s{0.0f};
    float surge_active_duration_s{1.25f};

    // Add spawns
    float add_period_s{10.0f};
    float add_timer_s{0.0f};
};

struct VulnerabilityWindow {
    bool  active{false};
    float timer_s{0.0f};
    float duration_s{6.0f};
    int   open_plate{-1};
};

struct BossData {
    Phase phase{Phase::P1};
    float health01{1.0f};
    float enrage_mult{1.0f};
    std::array<ArmorPlate, 4> plates{};
    AttackTimers atk{};
    VulnerabilityWindow vuln{};

    // Bookkeeping for arena gates so we only fire once
    bool gate_66_fired{false};
    bool gate_33_fired{false};
};

inline float clampf(float v, float lo, float hi) { return (v<lo?lo:(v>hi?hi:v)); }

inline int pick_first_open_plate(const BossData& b) {
    for (int i=0;i<static_cast<int>(b.plates.size());++i) {
        if (!b.plates[i].broken && !b.plates[i].open) return i;
    }
    // if all closed, pick any not broken
    for (int i=0;i<static_cast<int>(b.plates.size());++i) {
        if (!b.plates[i].broken) return i;
    }
    return -1;
}

// Params expected in CMechanicBinding entry for this behaviour (mutable at runtime by harness/scripts):
// {
//   "ram_hit": bool,                 // momentary true opens a plate window
//   "drill_contact": bool,           // true while player drilling weak plate
//   "turret_pressure": number,       // 0..1 additive reduction while window active
//   "dome_shield_active": bool,      // inside dome -> mitigates surge
//   "mission_tier": 1|2|3            // scales pace
// }
struct InputParams {
    bool ram_hit{false};
    bool drill_contact{false};
    float turret_pressure{0.0f};
    bool dome_shield_active{false};
    int mission_tier{1};
};

inline InputParams parse_params(const nlohmann::json& j){
    InputParams p{};
    if (j.is_object()){
        p.ram_hit = j.value("ram_hit", false);
        p.drill_contact = j.value("drill_contact", false);
        p.turret_pressure = static_cast<float>(j.value("turret_pressure", 0.0));
        p.dome_shield_active = j.value("dome_shield_active", false);
        p.mission_tier = j.value("mission_tier", 1);
        if (p.mission_tier < 1) p.mission_tier = 1;
        if (p.mission_tier > 3) p.mission_tier = 3;
        p.turret_pressure = clampf(p.turret_pressure, 0.0f, 1.0f);
    }
    return p;
}

} // namespace behemoth
