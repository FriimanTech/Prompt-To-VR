#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

namespace control_ext_helpers {

static std::string make_params_json_for_entry(const char* kind) {
    nlohmann::json j;
    // Provide minimal metadata the core may read; safe defaults
    if (std::string(kind)=="chain_tether") {
        j["asset_id"] = "construct_chain_tether";
        j["category"] = "Control";
        j["sustained"] = true;
    } else if (std::string(kind)=="restraining_bands") {
        j["asset_id"] = "construct_chain_tether"; // reuse material as bands
        j["category"] = "Control";
        j["instant"] = true;
    }
    return j.dump();
}

void ensure_core_control_entries(ptv::World& w, std::uint32_t hand_entity) {
    if (!w.valid(hand_entity)) return;
    if (!w.has<ptv::CMechanicBinding>(hand_entity)) return;
    auto& mb = w.get<ptv::CMechanicBinding>(hand_entity);

    auto ensure_one = [&](const std::string& id){
        bool exists=false;
        for (auto& en : mb.entries) if (en.id == id) { exists=true; break; }
        if (!exists) {
            ptv::CMechanicBinding::Entry en; en.id = id; en.builtin_id = ""; en.params_json = make_params_json_for_entry(id.c_str());
            mb.entries.push_back(std::move(en));
        }
    };

    // Register with construct core selection list by id
    ensure_one("chain_tether");
    ensure_one("restraining_bands");
}

} // namespace control_ext_helpers
