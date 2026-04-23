#include "ptv/save/SaveSystem.h"

#include <fstream>
#include <sstream>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"

namespace ptv {

nlohmann::json SaveSystem::save_world(const World& world) {
    nlohmann::json out = nlohmann::json::object();
    out["version"] = 1;
    auto view = world.registry().view<const CTransform>();
    for (auto entity : view) {
        const auto& t = view.get<const CTransform>(entity).t;
        nlohmann::json e = {
            {"id", static_cast<std::uint32_t>(entity)},
            {"position", {t.position.x, t.position.y, t.position.z}},
            {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}},
            {"scale", t.scale},
        };
        out["entities"].push_back(e);
    }
    return out;
}

void SaveSystem::load_world(const nlohmann::json& /*data*/, World& /*world*/) {
    // Intentional stub: scene is authoritative; savefile only overlays
    // per-entity state once the scene has been reloaded.
    PTV_INFO("SaveSystem::load_world() called (no-op in this skeleton)");
}

void SaveSystem::save_to_file(const std::string& path, const World& world) {
    std::ofstream out(path);
    if (!out) {
        PTV_WARN("Cannot write save file: {}", path);
        return;
    }
    out << save_world(world).dump(2);
}

void SaveSystem::load_from_file(const std::string& path, World& world) {
    std::ifstream in(path);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        load_world(nlohmann::json::parse(ss.str()), world);
    } catch (const std::exception& e) {
        PTV_WARN("Save file parse error: {}", e.what());
    }
}

}  // namespace ptv
