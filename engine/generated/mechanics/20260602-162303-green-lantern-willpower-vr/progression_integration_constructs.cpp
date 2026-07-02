#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>

#include "progression_shared.h"

namespace ptv {

// This TU intentionally only provides tiny helpers used by the register/mechanic file.

// Parse a convenience spend request tag to trigger an unlock attempt.
// Format: "progression.spend.request:{\"node\":\"id\"}"
std::optional<std::string> parse_spend_request(const std::string& tag) {
    constexpr std::string_view kPrefix = "progression.spend.request:";
    if (tag.rfind(kPrefix, 0) != 0) return std::nullopt;
    auto j = nlohmann::json::parse(tag.substr(kPrefix.size()), nullptr, false);
    if (!j.is_object() || !j.contains("node")) return std::nullopt;
    return j.at("node").get<std::string>();
}

// Parse a pin request: "progression.loadout.pin:{\"construct_id\":...,\"max\":6}"
std::optional<std::pair<std::string,int>> parse_pin_request(const std::string& tag) {
    constexpr std::string_view kPrefix = "progression.loadout.pin:";
    if (tag.rfind(kPrefix, 0) != 0) return std::nullopt;
    auto j = nlohmann::json::parse(tag.substr(kPrefix.size()), nullptr, false);
    if (!j.is_object() || !j.contains("construct_id")) return std::nullopt;
    int mx = std::max(1, j.value("max", 6));
    return std::make_pair(j.at("construct_id").get<std::string>(), mx);
}

// Parse XP from mission or objective events.
int parse_xp_if_any(const std::string& tag, std::string* event_id_out) {
    auto try_one = [&](std::string_view prefix)->int{
        if (tag.rfind(prefix, 0) != 0) return 0;
        auto j = nlohmann::json::parse(tag.substr(prefix.size()), nullptr, false);
        if (!j.is_object()) return 0;
        if (event_id_out && j.contains("id")) *event_id_out = j.at("id").get<std::string>();
        return j.value("xp", 0);
    };
    int xp = 0; xp += try_one("mission.complete:"); xp += try_one("objective.progress:");
    return xp;
}

// Parse persistence protocol tags
bool is_save_collect_start(const std::string& tag) {
    return tag.rfind("save.collect.start:", 0) == 0;
}
bool is_load_section_for_us(const std::string& tag) {
    constexpr std::string_view pfx = "load.section:";
    if (tag.rfind(pfx, 0) != 0) return false;
    auto j = nlohmann::json::parse(tag.substr(pfx.size()), nullptr, false);
    return j.is_object() && j.value("section_id", std::string()) == "progression";
}

} // namespace ptv
