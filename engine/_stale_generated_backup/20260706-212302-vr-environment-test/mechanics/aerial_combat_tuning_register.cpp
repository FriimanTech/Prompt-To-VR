#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "aerial_combat_tuning_shared.h"

namespace ptv {

class AerialCombatTuning : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        // Initialize local state
        CAerialAssistState st{};
        st.horizon_lock_default = true; // default to on in open space
        st.right_stick_altitude_mode = params.value("right_stick_altitude_mode", true);
        if (params.contains("emergency_brake_button")) st.emergency_brake_button = params.at("emergency_brake_button").get<std::string>();
        if (params.contains("boost_button")) st.boost_button = params.at("boost_button").get<std::string>();
        if (params.contains("altitude_axis")) st.altitude_axis = params.at("altitude_axis").get<std::string>();
        world.emplace_or_replace<CAerialAssistState>(entity, st);

        // Try an immediate apply based on current scene tags
        const bool open_space = scene_has_any_tag(world, {"orbital_battlefield", "oa_defense", "skybox", "flight", "flying"});
        apply_overrides_to_bindings(world, entity, st, open_space);
        auto st_now = st; st_now.open_space_active = open_space; st_now.applied_to_bindings = true;
        world.emplace_or_replace<CAerialAssistState>(entity, st_now);
    }

    void on_update(World& world, std::uint32_t entity, float /*dt*/) override {
        if (!world.valid(entity)) return;
        if (!world.has<CAerialAssistState>(entity)) return;
        auto st = world.get<CAerialAssistState>(entity);

        const bool open_space = scene_has_any_tag(world, {"orbital_battlefield", "oa_defense", "skybox", "flight", "flying"});
        bool changed = apply_overrides_to_bindings(world, entity, st, open_space);
        if (changed || st.open_space_active != open_space) {
            st.open_space_active = open_space;
            st.applied_to_bindings = true;
            world.emplace_or_replace<CAerialAssistState>(entity, st);
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Optional: leave overrides as-is; mechanic removal shouldn't yo-yo player controls mid-mission.
        (void)world; (void)entity;
    }

    std::string id() const override { return "generated.aerial_combat_tuning"; }
};

PTV_REGISTER_MECHANIC("generated.aerial_combat_tuning", AerialCombatTuning)

} // namespace ptv
