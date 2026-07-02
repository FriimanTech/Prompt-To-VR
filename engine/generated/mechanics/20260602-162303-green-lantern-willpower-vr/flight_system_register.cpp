#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <glm/glm.hpp>

#include <string>
#include <algorithm>

#include "flight_system_shared.h"

namespace ptv {

class FlightSystemMechanic : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        (void)w; (void)e;
        cfg_ = fs_parse_params(params);
        st_ = FlightStateInternal{};
        // Start in Hover by default
        st_.mode = FlightMode::Hover;
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;

        // Read inputs from this entity's binding entry each frame
        FlightInputs in = fs_read_inputs_from_binding(w, e, id());

        // Controls: compute target velocity & state intents
        fs_step_controls(cfg_, in, dt, st_);

        // Physics: integrate, handle OOB, collisions, enter/exit flight & hover
        fs_step_physics(cfg_, in, dt, w, e, st_);

        // Comfort & interop tags (stabilizer, vignette)
        fs_step_comfort(cfg_, in, dt, w, e, st_);
    }

    void on_detach(World& w, std::uint32_t e) override {
        // Ensure we leave with hover stable and comfort reset
        fs_publish_hover_enable(w, e, true);
        fs_publish_locomotion_velocity(w, e, glm::vec3(0.0f));
        fs_publish_aim_stabilizer(w, e, false, 0.0f);
        fs_publish_vignette(w, e, 0.0f);
    }

    std::string id() const override { return "generated.flight_system"; }

private:
    FlightParams cfg_{};
    FlightStateInternal st_{};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.flight_system", ptv::FlightSystemMechanic)
