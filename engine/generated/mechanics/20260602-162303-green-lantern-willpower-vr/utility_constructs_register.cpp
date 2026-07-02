#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "utility_constructs_shared.h"

namespace ptv {

void UtilityConstructs::on_attach(World& w, std::uint32_t e, const nlohmann::json& p) {
    // Defaults from params
    scanner_.interval_s = p.value("scanner_scan_interval_s", 2.0f);
    scanner_.range_m    = p.value("scanner_range_m", 18.0f);
    grabber_.pull_force = p.value("grabber_pull_force", 35.0f);
    repair_.rate_per_s  = p.value("repair_progress_rate", 0.25f);
    beacon_.intensity   = p.value("beacon_light_intensity", 1.0f);
    bubble_.ttl_s       = p.value("civilian_bubble_duration_s", 8.0f);
    bubble_.hp          = p.value("civilian_bubble_hp", 50.0f);
}

void UtilityConstructs::dismiss_all(World& w) {
    auto& reg = w.registry();
    auto kill = [&](entt::entity x){ if (x!=entt::null && reg.valid(x)) reg.destroy(x); };
    kill(scanner_.e); scanner_ = ScannerState{ };
    kill(grabber_.e); grabber_ = GrabberState{ };
    kill(beacon_.e);  beacon_  = BeaconState{ };
    kill(bubble_.e);  bubble_  = BubbleState{ };
    repair_ = RepairState{ };
}

void UtilityConstructs::on_update(World& w, std::uint32_t e, float dt) {
    // Read live binding params for input-like signals
    nlohmann::json p = read_self_params(w, e, id());

    // Global dismiss (B button per spec). Accept either dismiss:true or action:"dismiss"
    const bool do_dismiss = p.value("dismiss", false) || (p.value("action", std::string("")) == std::string("dismiss"));
    if (do_dismiss) {
        dismiss_all(w);
        // Soft haptics feedback (publish to dependency protocol)
        add_tag(w, static_cast<entt::entity>(e), "haptics.publish:{\"pattern_id\":\"hover_light\",\"hand\":\"right\",\"intensity_scale\":0.6,\"duration_s\":0.05}");
    }

    utility::update_scanner(w, e, *this, p, dt);
    utility::update_grabber(w, e, *this, p, dt);
    utility::update_repair (w, e, *this, p, dt);
    utility::update_beacon (w, e, *this, p, dt);
    utility::update_bubble (w, e, *this, p, dt);
}

void UtilityConstructs::on_detach(World& w, std::uint32_t) {
    dismiss_all(w);
}

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.utility_constructs", ptv::UtilityConstructs)
