#pragma once

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <cmath>

namespace ptv {

struct EvacZoneInfo {
    std::uint32_t entity{0};
    Vec3 position{0.0f,0.0f,0.0f};
    float radius_m{2.0f};
};

struct CivilianRescueState {
    bool initialized{false};
    bool is_civilian{false};
    bool is_evac_zone{false};

    // Escort
    bool escort_active{false};
    bool path_blocked{false};
    bool rescued{false};
    std::uint32_t target_evac{0};

    // Bubble (protective field hinting to utility_constructs)
    bool bubble_active{false};
    float bubble_hp{0.0f};
    float bubble_hp_max{50.0f};
    float bubble_ttl_s{10.0f};

    // Debris blocking cache
    Vec3 last_clear_dir{0.0f,0.0f,0.0f};

    // Cooldowns / UI
    float ui_hint_cooldown_s{0.0f};
};

// Forward declaration of main class so other .cpp can implement methods
class CivilianRescueInteractions : public IMechanic {
public:
    // IMechanic
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return "generated.civilian_rescue_interactions"; }

private:
    // Helpers
    static bool has_tag(World& w, std::uint32_t e, const std::string& tag);
    static void push_tag(World& w, std::uint32_t e, const std::string& s);
    static nlohmann::json read_binding_params(World& w, std::uint32_t e, const std::string& mech_id);

    void classify_and_cache(World& w, std::uint32_t e);
    void try_start_escort(World& w, std::uint32_t e, const nlohmann::json& p);
    void update_civilian(World& w, std::uint32_t e, float dt);
    void update_evac_marker(World& w, std::uint32_t e, float dt);

    std::optional<EvacZoneInfo> nearest_evac(World& w, const ptv::Vec3& pos) const;
    bool path_is_blocked_by_debris(World& w, const Vec3& a, const Vec3& b, float corridor_m, Vec3* out_push_dir);

    // Data
    std::unordered_map<std::uint32_t, CivilianRescueState> state_{};
    std::vector<EvacZoneInfo> evac_zones_{};

    // Tunables (read from first attach; can be overridden per-entity via params)
    float interact_radius_m_{2.0f};
    float escort_speed_mps_{1.5f};
    float hazard_avoid_radius_m_{1.0f};
    float debris_block_mass_kg_{8.0f};
    float debris_corridor_halfwidth_m_{0.75f};
    float bubble_default_hp_{50.0f};
    float bubble_default_ttl_s_{10.0f};
    float bubble_hazard_dps_{8.0f};
};

} // namespace ptv
