#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>

#include "progression_shared.h"

namespace ptv {

// Serialize our state into a compact JSON for save
nlohmann::json progression_serialize(const ProgressionState& s) {
    nlohmann::json j;
    j["total_xp_earned"] = s.total_xp_earned;
    j["unspent_xp"] = s.unspent_xp;
    j["unlocked_nodes"] = nlohmann::json::array();
    for (const auto& id : s.unlocked_nodes) j["unlocked_nodes"].push_back(id);
    j["pinned"] = s.pinned;
    return j;
}

void progression_publish_save_section(World& w, std::uint32_t e, const ProgressionState& s) {
    nlohmann::json sec{{"section_id","progression"},{"payload", progression_serialize(s)}};
    std::string ssec = std::string("save.section.push:") + sec.dump();
    if (w.has<CTag>(e)) { auto& tags = w.get<CTag>(e).tags; tags.push_back(ssec);} else { CTag t; t.tags.push_back(ssec); w.emplace_or_replace<CTag>(e, std::move(t)); }
}

// Load from JSON chunk
void progression_deserialize_into(ProgressionState& s, const nlohmann::json& j) {
    s.total_xp_earned = j.value("total_xp_earned", 0);
    s.unspent_xp = j.value("unspent_xp", 0);
    s.unlocked_nodes.clear();
    if (j.contains("unlocked_nodes") && j.at("unlocked_nodes").is_array())
        for (const auto& id : j.at("unlocked_nodes")) s.unlocked_nodes.insert(id.get<std::string>());
    s.pinned.clear();
    if (j.contains("pinned") && j.at("pinned").is_array())
        for (const auto& p : j.at("pinned")) s.pinned.push_back(p.get<std::string>());
}

// Publish the save.request to persist immediately (used on menu close or unlock commit)
void progression_request_save(World& w, std::uint32_t e) {
    std::string sreq = std::string("save.request:") + nlohmann::json({{"reason","progression_update"}}).dump();
    if (w.has<CTag>(e)) { auto& tags = w.get<CTag>(e).tags; tags.push_back(sreq);} else { CTag t; t.tags.push_back(sreq); w.emplace_or_replace<CTag>(e, std::move(t)); }
}

} // namespace ptv
