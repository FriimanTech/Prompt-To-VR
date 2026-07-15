#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <array>

namespace ptv {

struct HWMenuFlags {
    bool open{false};
    bool visible{false};
    bool fading_in{false};
    float alpha{0.0f};
    float fade_in_s{0.18f};
    float fade_out_s{0.16f};
};

enum class HWPanel : std::uint8_t { Mission=0, Constructs=1, Settings=2, Log=3 };

struct HWHudFlags {
    float ring_energy_01{1.0f};
    int mastery_pips{0};
    bool show_reticle{false};
    float damage_flash_s{0.0f};
};

struct HWPlacement {
    // Wrist/menu placement relative to player rig (meters, rig space)
    ptv::Vec3 left_wrist_offset{ -0.18f, -0.02f, 0.12f };
    ptv::Vec3 hud_forward_offset{ 0.0f, 0.02f, 0.42f };
    float menu_radius_m{0.26f};
    float menu_arc_deg{72.0f};
    bool follow_head_like{false}; // kept for parity with spec; not used
};

struct HWInputs {
    std::string open_menu_button{"X_left"};
    std::array<std::string,2> close_menu_buttons{ {"X_left", "B_right"} };
    bool open_menu_pulse{false};
    bool close_menu_pulse{false};
    bool dominant_ray_active{false};
    bool trigger_clicked{false};
    float ray_u{0.0f}; // 0..1 along the curved menu for selection
    bool aiming_ranged{false};
    float combat_intensity01{0.0f};
};

struct HWSavedPtrs { // cached targets inside the world
    std::uint32_t player_rig{0};
    bool player_rig_cached{false};
};

struct HWState {
    HWMenuFlags menu;
    HWHudFlags hud;
    HWPlacement place;
    HWInputs in;
    HWSavedPtrs cache;
    HWPanel current_panel{HWPanel::Mission};
    float suppress_timer_s{0.0f}; // auto-hide during intense combat
    float t_accum{0.0f};
};

// Helpers
std::uint32_t hw_find_player_rig(World& w, HWSavedPtrs& cache);
void hw_parse_params(const nlohmann::json& p, HWState& s);
void hw_tick_menu(HWState& s, float dt);
void hw_apply_visibility(World& w, std::uint32_t e, const HWState& s);
void hw_place_menu(World& w, std::uint32_t e, HWState& s, float dt);
void hw_tick_hud(HWState& s, float dt);

} // namespace ptv
