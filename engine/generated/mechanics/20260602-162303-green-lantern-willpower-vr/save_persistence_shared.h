#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <chrono>

#include <nlohmann/json.hpp>

#include "ptv/ecs/World.h"

namespace ptv {

// Tag protocol (strings in CTag.tags):
//   save.request:{"slot":"manual|auto|..."}
//   load.request:{"slot":"manual|auto|..."}
//   mission.complete:{...}              // triggers auto save
//   recharge.complete:{...}             // triggers auto save
//   save.register:{"section":"id"}
//   save.section.push:{"section":"id","data":{...}}
//
// Outputs (emitted on this mechanic's entity):
//   save.collect.start:{"slot":"...","window_s":float}
//   save.completed:{"slot":"...","bytes":N}
//   load.section:{"section":"id","data":{...}}
//   load.completed:{"slot":"...","sections":N}

struct TagMatch {
    std::string raw;              // full tag string kept for reference
    std::string prefix;           // e.g., "save.request:"
    nlohmann::json payload;       // parsed JSON after prefix
};

struct PendingBatch {
    bool active{false};
    std::string slot;
    float window_s{0.15f};
    float elapsed_s{0.0f};
};

// Per-session in-memory slot store (no disk IO in this stage)
class SaveStore {
public:
    static bool write(std::string_view slot, const nlohmann::json& j);
    static std::optional<nlohmann::json> read(std::string_view slot);
private:
    static std::unordered_map<std::string, nlohmann::json>& map();
};

// Helpers for tag scanning / mutation
namespace tags {
    // Returns and removes matching tags with given prefix from an entity's tag list
    std::vector<TagMatch> consume_with_prefix(World& w, std::uint32_t e, std::string_view prefix);

    // Global scan across all entities (CTag holders). Consumes matches in-place.
    std::vector<std::pair<std::uint32_t, TagMatch>> consume_all_with_prefix(World& w, std::string_view prefix);

    // Append one tag to an entity (creates/replaces CTag as needed and pushes string)
    void append(World& w, std::uint32_t e, const std::string& tag);
}

} // namespace ptv
