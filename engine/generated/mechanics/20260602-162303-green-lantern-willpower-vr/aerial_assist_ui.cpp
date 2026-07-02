#include "ptv/ecs/World.h"
#include "ptv/api/IMechanic.h"
#include <nlohmann/json.hpp>
#include <string>

#include "aerial_assist_shared.h"

namespace ptv {

// Utility that other modules/tests can call by sending params.enable true/false in our binding.
// Here we only expose a tiny helper to surface a toast when toggled, piggy-backing on HUD tag contract.
static constexpr const char* kTagToast = "ui.toast:";      // from HUD/Wrist dependency

static void push_tag(World& w, std::uint32_t e, const std::string& line){
    if (!w.has<CTag>(e)) w.emplace_or_replace<CTag>(e, CTag{});
    w.get<CTag>(e).tags.push_back(line);
}

// Called by register.cpp when params change to surface UX hints.
void aerial_assist_show_toggle_toast(World& w, std::uint32_t e, bool enabled){
    nlohmann::json j = {{"text", enabled ? "Combat Assist: ON" : "Combat Assist: OFF"}, {"style","info"}, {"ttl",1.2f}};
    push_tag(w, e, std::string(kTagToast) + j.dump());
}

} // namespace ptv
