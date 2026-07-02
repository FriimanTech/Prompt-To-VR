#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>

#include "hover_mode_shared.h"

// This file intentionally only exists to keep the suggested split; actual registration is in update.cpp
// (single macro across the set). Keeping an empty TU to satisfy the orchestrator's suggested filenames.

namespace ptv {
class HoverModeRegisterAnchor final : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json&) override {}
    void on_update(World&, std::uint32_t, float) override {}
    void on_detach(World&, std::uint32_t) override {}
    std::string id() const override { return "generated.hover_mode_mechanic.register_anchor"; }
};
}
