#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

using namespace utility_internal;

namespace ptv {

// Core tick implementation split across files; include-only TU provides definitions linkage
extern class UtilityConstructs;

void UtilityConstructs::tick_beacon(World&, std::uint32_t, const nlohmann::json&, float) {}
void UtilityConstructs::tick_bubble(World&, std::uint32_t, const nlohmann::json&, float) {}
void UtilityConstructs::tick_grabber(World&, std::uint32_t, const nlohmann::json&, float) {}
void UtilityConstructs::tick_repair(World&, std::uint32_t, const nlohmann::json&, float) {}
void UtilityConstructs::tick_scanner(World&, std::uint32_t, const nlohmann::json&, float) {}

} // namespace ptv
