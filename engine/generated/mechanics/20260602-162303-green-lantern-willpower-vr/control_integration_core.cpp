#include "ptv/ecs/World.h"
#include "control_constructs_shared.h"
#include <string>

namespace ptv {

// Best-effort registration shim: advertise our control entries to the construct core
// via tag-based message the core can pick up in its own update.
static void advertise_controls(World& w, std::uint32_t hand_e) {
    nlohmann::json j1; j1["id"] = "chain_tether"; j1["asset"] = "construct_chain_tether"; j1["category"] = "Control";
    ensure_tag(w, hand_e, std::string("construct_core.register_control:") + j1.dump());
    nlohmann::json j2; j2["id"] = "restraining_bands"; j2["asset"] = "construct_chain_tether"; j2["category"] = "Control";
    ensure_tag(w, hand_e, std::string("construct_core.register_control:") + j2.dump());
}

// Exposed helper so main mechanic can call once on attach
void construct_core_advertise_if_needed(World& w, std::uint32_t hand_e) {
    advertise_controls(w, hand_e);
}

} // namespace ptv
