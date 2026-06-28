#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>

// This file intentionally minimal: unlock tree logic is embedded in the main class.
// Placeholder kept for future expansion (e.g., tree validation, cost preview, UI feeds).

namespace ptv {

// Validate an unlock tree JSON object for basic structure. Returns true if sane.
static bool validate_unlock_tree(const nlohmann::json& tree) {
    if (!tree.is_object()) return false;
    for (auto it = tree.begin(); it != tree.end(); ++it) {
        if (!it.value().is_object()) return false;
        // Optional fields: cost (number), prereq (array of strings), effects (object)
        if (it.value().contains("cost") && !it.value().at("cost").is_number()) return false;
        if (it.value().contains("prereq") && !it.value().at("prereq").is_array()) return false;
        if (it.value().contains("effects") && !it.value().at("effects").is_object()) return false;
    }
    return true;
}

} // namespace ptv
