#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include <nlohmann/json.hpp>
#include <string>

#include "portal_shared.h"

namespace ptv {

class PortalTravel; // from runtime

} // namespace ptv

// The class implementation lives in portal_runtime.cpp; just include its declaration via ODR.
// We forward-declare here and register.

namespace ptv { class PortalTravel : public IMechanic { public: void on_attach(World&, std::uint32_t, const nlohmann::json&) override; void on_update(World&, std::uint32_t, float) override; void on_detach(World&, std::uint32_t) override; std::string id() const override; private: PortalConfig cfg_{}; PortalRuntime rt_{}; bool armed_announced_{false}; bool handoff_requested_via_param_{false}; float corridor_gate_timer_{0.0f}; static void ensure_tags(World&, std::uint32_t); void toast(World&, std::uint32_t, const std::string&); void show_portal_ready(World&, std::uint32_t); void publish_portal_activated(World&, std::uint32_t); void fade_out(World&, std::uint32_t, float); void request_scene_load(World&, std::uint32_t, const std::string&); bool edge_press(bool, bool&); void read_inputs_from_binding(World&, std::uint32_t); void consume_portal_request_if_any(World&, std::uint32_t); void reset_to_launch_idle(); }; }

PTV_REGISTER_MECHANIC("generated.portal_travel", ptv::PortalTravel)
