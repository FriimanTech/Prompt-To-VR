#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Companion lightweight hook to emit the first-time hint toast. Linked with the main class via TU-level statics.

namespace ptv {

// Forward declaration from runtime TU
static void post_wrist_hint_if_central(World& w, std::uint32_t e);

class LocomotionHintOnce : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json&) override {
        entity_ = e;
        // Mark that the hint capability exists (for HUD metrics) and post once in qualifying scene.
        CTag tag;
        if (w.has<CTag>(e)) tag = w.get<CTag>(e);
        tag.tags.push_back("hint_provider:locomotion_options");
        w.emplace_or_replace<CTag>(e, std::move(tag));
        post_wrist_hint_if_central(w, e);
    }
    void on_update(World&, std::uint32_t, float) override {}
    void on_detach(World&, std::uint32_t) override {}
    std::string id() const override { return "generated.locomotion_options_ui.hint_helper"; }
private:
    std::uint32_t entity_{0};
};

} // namespace ptv

// Note: No registration macro here (only one PTV_REGISTER_MECHANIC allowed for the core UI class).
