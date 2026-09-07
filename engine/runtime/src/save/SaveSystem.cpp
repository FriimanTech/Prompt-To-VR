#include "ptv/save/SaveSystem.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"

namespace ptv {
namespace {

/** Stable key for one entity.
 *
 * Deliberately NOT the raw entt entity id. Those are handles into a registry
 * and are reassigned on every load, so a save keyed on them restores the
 * wrong objects the moment anything about the scene changes -- which is
 * exactly the bug this file used to have. The scene's own string id is
 * stable by construction (see the world builder's id derivation).
 */
const std::string* stable_key(const World& world, entt::entity e) {
    if (world.registry().all_of<CInteractive>(e)) {
        return &world.registry().get<CInteractive>(e).id;
    }
    return world.id_of(e);
}

/** Has this transform moved from where the scene document put it?
 *
 *  Tolerance rather than exact equality: physics settles an object a
 *  fraction of a millimetre off its authored pose, and treating that as a
 *  change would write a delta for every prop in the level on every save. */
bool moved(const Transform& a, const Transform& b) {
    return glm::length(a.position - b.position) > 1.0e-3f ||
           std::fabs(glm::dot(a.rotation, b.rotation)) < 0.9999f ||
           std::fabs(a.scale - b.scale) > 1.0e-3f;
}

nlohmann::json transform_json(const Transform& t) {
    return nlohmann::json{
        {"position", {t.position.x, t.position.y, t.position.z}},
        {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}},
        {"scale", t.scale},
    };
}

}  // namespace

nlohmann::json SaveSystem::save_world(const World& world) {
    nlohmann::json out = nlohmann::json::object();
    out["version"] = 2;

    // --- Declared state, as DELTAS ------------------------------------
    //
    // Only what diverged from the world document. The document is
    // authoritative for everything nobody touched, so a save is a small set
    // of differences rather than a copy of the level -- and a world that was
    // regenerated with new content still loads, because unknown ids are
    // simply skipped on the way back in.
    nlohmann::json interactives = nlohmann::json::object();
    for (auto e : world.registry().view<const CInteractive>()) {
        const auto& ci = world.registry().get<const CInteractive>(e);
        nlohmann::json deltas = nlohmann::json::object();
        for (const auto& [key, value] : ci.state) {
            const auto was = ci.pristine.find(key);
            if (was != ci.pristine.end() && was->second == value) continue;
            switch (value.type) {
                case InteractiveValue::Type::Bool:
                    deltas[key] = {{"type", "bool"}, {"value", value.b}};
                    break;
                case InteractiveValue::Type::Number:
                    deltas[key] = {{"type", "number"}, {"value", value.number}};
                    break;
                case InteractiveValue::Type::Name:
                    deltas[key] = {{"type", "name"}, {"value", value.name}};
                    break;
                case InteractiveValue::Type::Vector:
                default:
                    deltas[key] = {
                        {"type", "vector"},
                        {"value",
                         {value.vector.x, value.vector.y, value.vector.z}}};
                    break;
            }
        }
        if (ci.locked) deltas["__locked"] = true;
        if (!deltas.empty()) interactives[ci.id] = std::move(deltas);
    }
    if (!interactives.empty()) out["interactives"] = std::move(interactives);

    // --- Movable objects that actually moved ---------------------------
    nlohmann::json moved_objects = nlohmann::json::object();
    for (auto e : world.registry().view<const CTransform, const CMovable>()) {
        const auto& mv = world.registry().get<const CMovable>(e);
        const auto& t = world.registry().get<const CTransform>(e).t;
        if (!moved(t, mv.pristine)) continue;
        const std::string* key = stable_key(world, e);
        if (key == nullptr || key->empty()) continue;
        moved_objects[*key] = transform_json(t);
    }
    if (!moved_objects.empty()) out["moved"] = std::move(moved_objects);

    // --- Equipped items ------------------------------------------------
    //
    // Recorded here rather than by builtin.equip, so loadout and outfit
    // survive a transition without any mechanic writing save code.
    nlohmann::json equipped = nlohmann::json::array();
    for (auto e : world.registry().view<const CEquipped>()) {
        const auto& eq = world.registry().get<const CEquipped>(e);
        if (eq.wearer == 0xFFFFFFFFu || eq.slot.empty()) continue;
        const std::string* item = stable_key(world, e);
        const std::string* wearer =
            world.id_of(static_cast<entt::entity>(eq.wearer));
        if (item == nullptr || wearer == nullptr) continue;
        equipped.push_back(nlohmann::json{
            {"item", *item},
            {"wearer", *wearer},
            {"slot", eq.slot},
            {"removable", eq.removable},
            {"re_equippable", eq.re_equippable},
        });
    }
    if (!equipped.empty()) out["equipped"] = std::move(equipped);

    return out;
}

void SaveSystem::load_world(const nlohmann::json& data, World& world) {
    if (!data.is_object()) return;

    // The scene has already been rebuilt from its document at this point, so
    // this only overlays the differences. Unknown ids are skipped rather
    // than treated as errors: a world regenerated with different content
    // must still load an old save, just with less of it restored.
    int applied = 0;
    int skipped = 0;

    if (data.contains("interactives") && data["interactives"].is_object()) {
        for (const auto& [id, deltas] : data["interactives"].items()) {
            entt::entity target = entt::null;
            for (auto e : world.registry().view<CInteractive>()) {
                if (world.registry().get<CInteractive>(e).id == id) {
                    target = e;
                    break;
                }
            }
            if (target == entt::null || !deltas.is_object()) {
                ++skipped;
                continue;
            }
            auto& ci = world.registry().get<CInteractive>(target);
            for (const auto& [key, value] : deltas.items()) {
                if (key == "__locked") {
                    ci.locked = value.is_boolean() && value.get<bool>();
                    continue;
                }
                if (!value.is_object() || !value.contains("value")) continue;
                const std::string type = value.value("type", std::string());
                const auto& v = value["value"];
                if (type == "bool" && v.is_boolean()) {
                    ci.state[key] = InteractiveValue::of(v.get<bool>());
                } else if (type == "number" && v.is_number()) {
                    ci.state[key] = InteractiveValue::of(v.get<float>());
                } else if (type == "name" && v.is_string()) {
                    ci.state[key] =
                        InteractiveValue::named(v.get<std::string>());
                } else if (type == "vector" && v.is_array() &&
                           v.size() >= 3) {
                    ci.state[key] = InteractiveValue::of(
                        Vec3(v[0].get<float>(), v[1].get<float>(),
                             v[2].get<float>()));
                }
            }
            ++applied;
        }
    }

    if (data.contains("moved") && data["moved"].is_object()) {
        for (const auto& [id, tj] : data["moved"].items()) {
            const auto e = world.find(id);
            if (e == entt::null || !world.registry().all_of<CTransform>(e)) {
                ++skipped;
                continue;
            }
            auto& t = world.registry().get<CTransform>(e).t;
            if (tj.contains("position") && tj["position"].is_array() &&
                tj["position"].size() >= 3) {
                const auto& p = tj["position"];
                t.position = Vec3(p[0].get<float>(), p[1].get<float>(),
                                  p[2].get<float>());
            }
            if (tj.contains("rotation") && tj["rotation"].is_array() &&
                tj["rotation"].size() >= 4) {
                const auto& r = tj["rotation"];
                t.rotation = glm::normalize(
                    Quat(r[3].get<float>(), r[0].get<float>(),
                         r[1].get<float>(), r[2].get<float>()));
            }
            if (tj.contains("scale") && tj["scale"].is_number()) {
                t.scale = tj["scale"].get<float>();
            }
            ++applied;
        }
    }

    PTV_INFO("SaveSystem::load_world overlaid {} record(s), skipped {}",
             applied, skipped);
}

void SaveSystem::save_to_file(const std::string& path, const World& world) {
    // Atomic write: a crash partway through must not leave a corrupt save
    // where a valid one used to be. Write a sibling temp file, then rename
    // over the target, which is atomic on both platforms this ships to.
    namespace fs = std::filesystem;
    const fs::path target(path);
    const fs::path temp = fs::path(path + ".tmp");
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            PTV_WARN("Cannot write save file: {}", path);
            return;
        }
        out << save_world(world).dump(2);
        if (!out) {
            PTV_WARN("Save write failed: {}", path);
            return;
        }
    }
    fs::rename(temp, target, ec);
    if (ec) {
        // Some filesystems refuse rename-over; fall back to remove + rename
        // rather than losing the save entirely.
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
        if (ec) PTV_WARN("Could not finalize save file: {}", path);
    }
}

void SaveSystem::load_from_file(const std::string& path, World& world) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        const auto parsed = nlohmann::json::parse(ss.str());
        const int version = parsed.value("version", 1);
        if (version > 2) {
            PTV_WARN("Save file version {} is newer than this build "
                     "understands; ignoring it", version);
            return;
        }
        load_world(parsed, world);
    } catch (const std::exception& e) {
        PTV_WARN("Save file parse error: {}", e.what());
    }
}

}  // namespace ptv
