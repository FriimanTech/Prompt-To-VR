#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <vector>

namespace ptv {

// Forward declares from deps (signatures exported by mission_system / hud).
namespace mission_api {
    std::string current_mission_id();
    bool mission_in_progress();
}
namespace hud_helpers {
    bool hud_get_settings_json(std::string& out_json); // best-effort; may be unused here
}

class PortalTravel final : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return "generated.portal_travel"; }
private:
    enum class Phase : std::uint8_t { IdleAtLaunch, Arming, FadingOut, InCorridor, Cancelling, Arrived };

    // runtime state (per-bound-entity)
    Phase phase_{Phase::IdleAtLaunch};
    float t_{0.0f};
    float arm_time_s_{0.5f};
    float fade_time_s_{0.6f};
    float corridor_min_time_s_{3.0f};
    bool practice_mode_{true};
    int practice_max_alive_{4};
    bool practice_bound_{false};
    std::string target_scene_; // e.g. from params or inferred later
    std::string mission_name_cache_;

    // helpers
    static bool j_get_bool(const nlohmann::json& j, const char* k, bool def_v);
    static int  j_get_int(const nlohmann::json& j, const char* k, int def_v);
    static std::string j_get_str(const nlohmann::json& j, const char* k, const std::string& def_v);

    static void ensure_status_tag(World& w, std::uint32_t e, const std::string& status);
    static void ensure_tag(World& w, std::uint32_t e, const std::string& tag);
    static void drop_tag(World& w, std::uint32_t e, const std::string& tag);

    static void ensure_practice_spawner_binding(World& w, std::uint32_t e, int max_alive);
    static void remove_practice_spawner_binding(World& w, std::uint32_t e);
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.portal_travel", ptv::PortalTravel)
