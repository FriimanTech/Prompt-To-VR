#pragma once
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <array>
#include <optional>
#include <unordered_map>

namespace ptv {

// Local enum for heavy forms
enum class HeavyForm : std::uint8_t {
    None = 0,
    Turret,
    MechArm,
    JetDrill,
    FistSwarm
};

struct HeavyParams {
    float willpower_focus_required{0.6f};
    float cooldown_s{20.0f};
    float cost_turret{8.0f};
    float cost_mech_arm{10.0f};
    float cost_jet_drill{12.0f};
    float cost_fist_swarm{10.0f};
};

struct PerFormCooldowns {
    float turret{0.0f};
    float mech_arm{0.0f};
    float jet_drill{0.0f};
    float fist_swarm{0.0f};
};

struct ChargeState {
    bool charging{false};
    float t{0.0f};             // seconds accumulated while holding RT
    float preview_intensity{0.0f}; // [0..1] for VFX intensity
    HeavyForm form{HeavyForm::None};
};

struct HeavyState {
    HeavyParams cfg;              // configured (from params_json on attach)
    ChargeState charge;           // current charge preview state
    PerFormCooldowns cd;          // cooldown timers remaining
    float last_focus_sample{0.0f};
    // smoothing for two-hand stabilization (applies to preview intensity only here)
    float stab_smoother{0.0f}; // approaches 1 while left grip held
};

// Utility: safe JSON bool read
inline bool json_bool(const nlohmann::json& j, const char* key, bool defv=false) {
    if (!j.is_object() || !j.contains(key)) return defv;
    const auto& v = j.at(key);
    return v.is_boolean() ? v.get<bool>() : defv;
}

// Utility: safe JSON float read
inline float json_float(const nlohmann::json& j, const char* key, float defv=0.0f) {
    if (!j.is_object() || !j.contains(key)) return defv;
    const auto& v = j.at(key);
    return v.is_number() ? v.get<float>() : defv;
}

// Parse active heavy form string -> enum
inline HeavyForm parse_form(std::string_view s) {
    if (s == "construct_turret") return HeavyForm::Turret;
    if (s == "construct_mech_arm") return HeavyForm::MechArm;
    if (s == "construct_energy_jet_drill") return HeavyForm::JetDrill;
    if (s == "construct_fist_swarm") return HeavyForm::FistSwarm;
    return HeavyForm::None;
}

// Tag helpers (publish strings consumed by other systems)
inline void publish_haptics(World& w, std::uint32_t e, const char* pattern_id, const char* hand, float intensity, float dur_s) {
    if (!w.valid(e)) return;
    ptv::CTag tag; tag.tags.reserve(1);
    nlohmann::json j{{"pattern_id", pattern_id},{"hand", hand},{"intensity_scale", intensity},{"duration_s", dur_s}};
    tag.tags.emplace_back(std::string("haptics.publish:") + j.dump());
    w.emplace_or_replace<ptv::CTag>(e, std::move(tag));
}

inline void add_tag(World& w, std::uint32_t e, const std::string& s) {
    if (!w.valid(e)) return;
    ptv::CTag t;
    t.tags.push_back(s);
    // Merge if already exists
    if (w.has<ptv::CTag>(e)) {
        auto existing = w.get<ptv::CTag>(e);
        for (auto& x : existing.tags) t.tags.push_back(x);
    }
    w.emplace_or_replace<ptv::CTag>(e, std::move(t));
}

// VFX charge preview tag
inline void publish_charge_vfx(World& w, std::uint32_t hand_e, float intensity01) {
    nlohmann::json j{{"asset","construct_energy_vfx_core"},{"mode","charge"},{"intensity", intensity01}};
    add_tag(w, hand_e, std::string("vfx.spawn:") + j.dump());
}

// HUD cooldowns tag
inline void publish_cooldowns(World& w, std::uint32_t e, const PerFormCooldowns& cd) {
    nlohmann::json j{{"heavy_cooldowns",{{"turret", cd.turret},{"mech_arm", cd.mech_arm},{"jet_drill", cd.jet_drill},{"fist_swarm", cd.fist_swarm}}}};
    add_tag(w, e, std::string("hud.cooldown:") + j.dump());
}

// Ring resource consumption tags
inline void publish_consume_once(World& w, std::uint32_t e, float amount) {
    nlohmann::json j{{"energy", amount}};
    add_tag(w, e, std::string("ring.consume:") + j.dump());
}
inline void publish_drain_per_s(World& w, std::uint32_t spawned_e, float dps) {
    nlohmann::json j{{"drain_per_s", dps}};
    add_tag(w, spawned_e, std::string("ring.drain_per_s:") + j.dump());
}

// Armor break broadcast for Jet/Drill
inline void publish_armor_break(World& w, std::uint32_t e, float mult) {
    nlohmann::json j{{"multiplier", mult}};
    add_tag(w, e, std::string("combat.armor_break_multiplier:") + j.dump());
}

// Construct spawn broadcast; actual autonomous behaviour handled by AI/other systems.
inline void broadcast_spawn(World& w, std::uint32_t hand_e, const char* kind, const char* asset_id) {
    nlohmann::json j{{"kind", kind},{"asset", asset_id},{"origin_entity", hand_e}};
    add_tag(w, hand_e, std::string("construct.spawn:") + j.dump());
}

} // namespace ptv
