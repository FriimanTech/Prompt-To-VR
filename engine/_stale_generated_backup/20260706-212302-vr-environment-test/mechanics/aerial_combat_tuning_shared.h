#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace ptv {
class World;

// Local component to track assist state and avoid churn
struct CAerialAssistState {
    bool open_space_active{false};
    bool horizon_lock_default{true};
    bool right_stick_altitude_mode{true};
    bool applied_to_bindings{false};
    // Caching strings for buttons/axes so acceptance can assert
    std::string emergency_brake_button{"B_right_tap"};
    std::string boost_button{"LS_click"};
    std::string altitude_axis{"R_stick_y"};
};

// Helpers
bool scene_has_any_tag(World& world, const std::vector<std::string>& tags);

// Apply or clear overrides on the given entity's CMechanicBinding for flight + constructs
// Returns true if any params were changed (JSON mutated)
bool apply_overrides_to_bindings(World& world,
                                 std::uint32_t entity,
                                 const CAerialAssistState& desired,
                                 bool enable_open_space);

} // namespace ptv
