#include "save_persistence_shared.h"

#include <unordered_map>

namespace ptv {

static std::unordered_map<std::string, nlohmann::json> g_slots; // process-local

std::unordered_map<std::string, nlohmann::json>& SaveStore::map() {
    return g_slots;
}

bool SaveStore::write(std::string_view slot, const nlohmann::json& j) {
    map()[std::string(slot)] = j;
    return true;
}

std::optional<nlohmann::json> SaveStore::read(std::string_view slot) {
    auto it = map().find(std::string(slot));
    if (it == map().end()) return std::nullopt;
    return it->second;
}

} // namespace ptv
