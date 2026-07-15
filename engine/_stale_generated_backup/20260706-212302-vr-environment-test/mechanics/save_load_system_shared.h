#pragma once

#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <optional>

namespace ptv {

// Public lightweight API so other tasks can query/update the profile and request saves.
// Note: No external persistence layer exists; this task emulates persistence in-memory
// and mirrors the latest blob onto the bound entity's CTag for test harness visibility.

// Read a value at a dotted path (e.g. "constructs.unlocked"). Returns null if absent.
nlohmann::json ptv_profile_get(World& world, std::uint32_t entity, const std::string& dotted_path);

// Set a value at a dotted path, creating intermediate objects as needed.
void ptv_profile_set(World& world, std::uint32_t entity, const std::string& dotted_path,
                     const nlohmann::json& value);

// Convenience: namespaced settings get/set (e.g. ns="locomotion", key="mode").
nlohmann::json ptv_settings_get(World& world, std::uint32_t entity,
                                const std::string& name_space,
                                const std::string& key,
                                const nlohmann::json& default_value = nlohmann::json());

void ptv_settings_set(World& world, std::uint32_t entity,
                      const std::string& name_space,
                      const std::string& key,
                      const nlohmann::json& value);

// Immediate save with reason tag for analytics/debug.
void ptv_save_now(World& world, std::uint32_t entity, const std::string& reason);

// Returns the current in-memory saved profile blob (for diagnostics/tests).
const nlohmann::json& ptv_profile_current();

} // namespace ptv
