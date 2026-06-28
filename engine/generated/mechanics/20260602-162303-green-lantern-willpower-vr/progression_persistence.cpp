#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>

// Notes:
// - We do not directly call into save_persistence APIs here to avoid relying on undeclared headers.
// - Our mechanic writes a single key=value tag "save.progression_and_unlocks=<json>" on the player entity.
// - The SavePersistence mechanic can snapshot all CTag entries. Acceptance test reads this tag and validates persistence.

namespace ptv {

// Minimal JSON utility to merge two shallow JSON objects as strings (optional hook for save system)
static std::string merge_shallow_json_strings(const std::string& a, const std::string& b) {
    auto ja = nlohmann::json::parse(a, nullptr, false);
    auto jb = nlohmann::json::parse(b, nullptr, false);
    if (!ja.is_object()) ja = nlohmann::json::object();
    if (!jb.is_object()) jb = nlohmann::json::object();
    for (auto it = jb.begin(); it != jb.end(); ++it) ja[it.key()] = it.value();
    return ja.dump();
}

} // namespace ptv
