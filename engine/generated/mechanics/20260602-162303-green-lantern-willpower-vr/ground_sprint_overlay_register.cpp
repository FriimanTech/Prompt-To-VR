#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "ground_sprint_overlay_shared.h"

namespace ptv {

// Declaration only; implemented in ground_sprint_overlay_update.cpp
class GroundSprintOverlay : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override;
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.ground_sprint_overlay", ptv::GroundSprintOverlay)
