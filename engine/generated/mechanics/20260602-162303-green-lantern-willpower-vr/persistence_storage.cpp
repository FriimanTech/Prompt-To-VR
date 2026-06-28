#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"

#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>

// This file intentionally only holds helper symbols in an internal namespace to
// keep translation units small and speed up incremental builds. Public surface
// remains the mechanic registered in persistence_register.cpp.

namespace ptv { namespace persistence_detail {

struct KVStore {
    std::unordered_map<std::string, nlohmann::json> kv; // debug scratch area if needed
    static KVStore& instance() { static KVStore s; return s; }
};

// Utility to stash tiny debug payloads addressed by keys, for diagnostics in tests.
inline void set_debug_blob(const std::string& key, const nlohmann::json& j) {
    KVStore::instance().kv[key] = j;
}

inline nlohmann::json get_debug_blob(const std::string& key) {
    auto& kv = KVStore::instance().kv;
    auto it = kv.find(key);
    return it == kv.end() ? nlohmann::json{} : it->second;
}

} } // namespace ptv::persistence_detail
