#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "flight_mode_mechanic_shared.h"

namespace ptv {

void FlightModeMechanic::on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) {
    base_speed_mps_    = params.value("base_speed_mps",    5.0f);
    boost_max_mps_     = params.value("boost_max_speed_mps", 12.0f);
    accel_tau_s_       = params.value("accel_tau_s", 0.18f);
    decel_tau_s_       = params.value("decel_tau_s", 0.22f);
    altitude_rate_mps_ = params.value("altitude_rate_mps", 2.0f);
    right_stick_altitude_ = params.value("right_stick_altitude_mode", true);
    bank_max_deg_      = params.value("bank_max_deg", 20.0f);
    horizon_lock_      = params.value("horizon_lock", true);
    drain_per_s_       = params.value("drain_per_s", 1.0f);
    boost_extra_drain_ = params.value("boost_extra_drain_per_s", 3.0f);

    // Ensure a CFlightState exists
    world.emplace_or_replace<CFlightState>(entity, CFlightState{});
}

void FlightModeMechanic::on_detach(World& world, std::uint32_t entity) {
    // Clear state on detach
    if (world.has<CFlightState>(entity)) {
        auto st = world.get<CFlightState>(entity);
        (void)st; // no-op; component removal optional
    }
}

PTV_REGISTER_MECHANIC("generated.flight_mode_mechanic", FlightModeMechanic)

} // namespace ptv
