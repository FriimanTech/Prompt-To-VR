#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <cmath>
#include "enemy_ai_shared.h"

// Declarations of per-role tickers
namespace ptv { void ai_tick_ground(World&, std::uint32_t, PerEntityState&, const struct GroundParams&, float); }
namespace ptv { void ai_tick_drone(World&, std::uint32_t, PerEntityState&, const struct DroneParams&, float); }
namespace ptv { void ai_tick_turret(World&, std::uint32_t, PerEntityState&, const struct TurretParams&, float); }
namespace ptv { void ai_tick_brute(World&, std::uint32_t, PerEntityState&, const struct BruteParams&, float); }

namespace ptv {

// Mirror param structs here (same layout) to avoid separate header exposure
struct GroundParams { float move_speed{3.5f}; float burst_rate_hz{6.0f}; int burst_size{3}; float flank_distance{4.0f}; };
struct DroneParams { float strafe_speed{6.0f}; float juke_period{0.9f}; float altitude_bias{1.5f}; float band_min{8.0f}; float band_max{16.0f}; };
struct TurretParams { float snipe_rate_hz{0.8f}; float telegraph_time{0.8f}; };
struct BruteParams  { float rush_speed{9.0f}; float aura_radius{5.0f}; float aura_mult{1.35f}; float windup_time{0.7f}; };

class EnemyAICombatPack : public IBehaviour {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        PerEntityState st{};
        st.seed = 0.37f + 0.000123f * static_cast<float>(e);
        // Difficulty by mission_tier (1..5)
        int tier = std::clamp(params.value("mission_tier", 1), 1, 5);
        st.difficulty = 0.8f + 0.3f * static_cast<float>(tier);

        // Detect role
        st.role = EnemyRole::Ground;
        if (w.has<CMesh>(e)) {
            const auto& m = w.get<CMesh>(e);
            const std::string& a = m.asset_id;
            if (a.find("drone") != std::string::npos) st.role = EnemyRole::Drone;
            else if (a.find("turret") != std::string::npos) st.role = EnemyRole::Turret;
            else if (a.find("brute") != std::string::npos)  st.role = EnemyRole::Brute;
        }
        if (w.has<CTag>(e)) {
            const auto& ct = w.get<CTag>(e);
            for (const auto& s : ct.tags) {
                if (s == "aerial") st.role = EnemyRole::Drone;
                if (s == "turret") st.role = EnemyRole::Turret;
                if (s == "elite")  st.role = EnemyRole::Brute;
            }
        }

        // Role-specific tunables (allow json override)
        if (st.role == EnemyRole::Drone) {
            drone_params_.band_min = params.value("drone_band_min", 8.0f);
            drone_params_.band_max = params.value("drone_band_max", 16.0f);
        }
        if (st.role == EnemyRole::Brute) {
            brute_params_.aura_radius = params.value("brute_aura_radius", 5.0f);
            brute_params_.aura_mult   = params.value("brute_aura_mult", 1.35f);
        }

        states_.emplace(e, st);
    }

    void on_tick(World& w, std::uint32_t e, float dt) override {
        auto it = states_.find(e);
        if (it == states_.end()) return;
        auto& st = it->second;

        switch (st.role) {
            case EnemyRole::Ground: ai_tick_ground(w, e, st, ground_params_, dt); break;
            case EnemyRole::Drone:  ai_tick_drone (w, e, st, drone_params_,  dt); break;
            case EnemyRole::Turret: ai_tick_turret(w, e, st, turret_params_, dt); break;
            case EnemyRole::Brute:  ai_tick_brute (w, e, st, brute_params_,  dt); break;
        }
    }

    void on_detach(World&, std::uint32_t e) override {
        states_.erase(e);
    }

    std::string id() const override { return "generated.enemy_ai_combat_pack"; }

private:
    std::unordered_map<std::uint32_t, PerEntityState> states_;

    GroundParams ground_params_{};
    DroneParams  drone_params_{};
    TurretParams turret_params_{};
    BruteParams  brute_params_{};
};

} // namespace ptv

PTV_REGISTER_BEHAVIOUR("generated.enemy_ai_combat_pack", ptv::EnemyAICombatPack)
