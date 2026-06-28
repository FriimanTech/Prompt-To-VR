#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Helper routines for future section-merging policies.
// Kept separate so other mechanics can include this TU at link-time without
// pulling in the main SavePersistence implementation twice.

namespace ptv { namespace persistence_sections {

// Merge policy: prefer incoming keys, keep missing ones.
inline nlohmann::json merge_shallow(const nlohmann::json& base, const nlohmann::json& incoming) {
    if (!base.is_object() && !incoming.is_object()) return incoming;
    nlohmann::json out = base.is_object() ? base : nlohmann::json::object();
    if (incoming.is_object()) {
        for (auto it = incoming.begin(); it != incoming.end(); ++it) {
            out[it.key()] = it.value();
        }
    }
    return out;
}

// Optional: encode a tiny digest for quick equality checks in tests.
inline std::string tiny_digest(const nlohmann::json& j) {
    if (!j.is_object()) return "0";
    std::size_t acc = 1469598103934665603ull; // FNV offset
    for (auto it = j.begin(); it != j.end(); ++it) {
        for (char c : it.key()) acc ^= static_cast<unsigned char>(c), acc *= 1099511628211ull;
        if (it.value().is_string()) {
            const auto& s = it.value().get_ref<const std::string&>();
            for (char c : s) acc ^= static_cast<unsigned char>(c), acc *= 1099511628211ull;
        } else if (it.value().is_number()) {
            auto s = it.value().dump();
            for (char c : s) acc ^= static_cast<unsigned char>(c), acc *= 1099511628211ull;
        } else {
            auto s = it.value().dump(0);
            for (char c : s) acc ^= static_cast<unsigned char>(c), acc *= 1099511628211ull;
        }
    }
    return std::to_string(static_cast<unsigned long long>(acc & 0xffffffffull));
}

} } // namespace ptv::persistence_sections
