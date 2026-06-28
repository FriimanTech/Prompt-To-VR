#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <optional>
// [ptv-bounds] removed disallowed include: utility_constructs_shared.h

namespace ptv {

class UtilityConstructs : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json& params) override {
        // Defaults; per-frame update reads live params from binding entry
        scanner_.scan_interval = params.value("scanner_scan_interval_s", 2.0f);
        scanner_.range = params.value("scanner_range_m", 18.0f);
        grabber_.pull_force = params.value("grabber_pull_force", 35.0f);
        repair_.rate = params.value("repair_progress_rate", 0.25f);
        beacon_.intensity = params.value("beacon_light_intensity", 1.0f);
        bubble_.default_duration = params.value("civilian_bubble_duration_s", 8.0f);
        bubble_.default_hp = params.value("civilian_bubble_hp", 50.0f);
    }

    void on_update(World& w, std::uint32_t ring_entity, float dt) override;
    void on_detach(World& w, std::uint32_t) override {
        cleanup_spawned(w);
    }
    std::string id() const override { return "generated.utility_constructs"; }

private:
    // Feature entry points implemented in other translation units
    void tick_scanner(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt);
    void tick_grabber(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt);
    void tick_repair(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt);
    void tick_beacon(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt);
    void tick_bubble(World& w, std::uint32_t ring_entity, const nlohmann::json& live, float dt);

    void cleanup_spawned(World& w);

    utility_internal::ScannerState scanner_{};
    utility_internal::GrabberState grabber_{};
    utility_internal::RepairState repair_{};
    utility_internal::BeaconState beacon_{};
    utility_internal::BubbleState bubble_{};
};

void UtilityConstructs::on_update(World& w, std::uint32_t ring_entity, float dt) {
    // Read live params_json from our binding entry if present
    nlohmann::json live{};
    if (w.has<CMechanicBinding>(ring_entity)) {
        auto& mb = w.get<CMechanicBinding>(ring_entity);
        for (const auto& e : mb.entries) {
            if (e.id == id()) {
                live = nlohmann::json::parse(e.params_json, nullptr, false);
                break;
            }
        }
    }
    // Dismiss input shared across constructs
    const bool dismiss_pressed = live.is_object() ? live.value("b_button_dismiss", false) : false;
    if (dismiss_pressed) {
        cleanup_spawned(w);
        // Soft feedback tag to ring for tests
        if (w.valid(ring_entity)) utility_internal::add_tag(w, ring_entity, "utility:dismiss");
    }

    tick_scanner(w, ring_entity, live, dt);
    tick_grabber(w, ring_entity, live, dt);
    tick_repair(w, ring_entity, live, dt);
    tick_beacon(w, ring_entity, live, dt);
    tick_bubble(w, ring_entity, live, dt);
}

void UtilityConstructs::cleanup_spawned(World& w) {
    auto& reg = w.registry();
    if (scanner_.drone != entt::null && reg.valid(scanner_.drone)) reg.destroy(scanner_.drone);
    scanner_.drone = entt::null; scanner_.t_accum = 0.0f;
    if (grabber_.hand != entt::null && reg.valid(grabber_.hand)) reg.destroy(grabber_.hand);
    grabber_.hand = entt::null; grabber_.latched = entt::null;
    if (beacon_.beacon != entt::null && reg.valid(beacon_.beacon)) reg.destroy(beacon_.beacon);
    beacon_.beacon = entt::null; beacon_.head_rot_t = 0.0f;
    repair_.current = entt::null; repair_.progress.clear();
}

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.utility_constructs", ptv::UtilityConstructs)
