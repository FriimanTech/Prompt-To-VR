#pragma once

// Offline world-generation host support: registry access + scene serialization.
// This library links NO engine core (no Vulkan/OpenXR/Jolt/renderer/ECS). It
// only runs an IWorldBuilder and turns its WorldContext calls into scene JSON.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ptv/api/IWorldBuilder.h"

namespace ptv::world {

// Ids of every builder registered via PTV_REGISTER_WORLD_BUILDER at static
// init (excludes the internal fallback unless include_fallback is true).
std::vector<std::string> registered_builder_ids(bool include_fallback = false);

// Build an instance of the registered builder with the given id, or nullptr.
std::unique_ptr<IWorldBuilder> make_builder(const std::string& id);

// Run ``builder`` against a fresh WorldContext and return the serialized
// ``world.scene.json`` document. ``asset_map`` maps asset_id -> glb_path for
// place_object() calls (may be empty).
nlohmann::json build_scene_json(IWorldBuilder& builder, std::uint64_t seed,
                                const nlohmann::json& asset_map);

// Id used for the always-available deterministic fallback builder.
inline constexpr const char* kFallbackBuilderId = "builtin.world.fallback";

}  // namespace ptv::world
