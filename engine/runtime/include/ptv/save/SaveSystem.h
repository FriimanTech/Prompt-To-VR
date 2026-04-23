#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace ptv {

class World;

/** Minimal save / load facade.
 *
 * Built-in mechanics register entries into the returned JSON object in
 * ``save_world`` and read them back in ``load_world``. We intentionally do
 * not persist the ECS layout; instead we rely on the scene being reloaded
 * from ``scene.json`` and then overlaying any persisted per-entity state.
 */
class SaveSystem {
public:
    static nlohmann::json save_world(const World& world);
    static void load_world(const nlohmann::json& data, World& world);

    static void save_to_file(const std::string& path, const World& world);
    static void load_from_file(const std::string& path, World& world);
};

}  // namespace ptv
