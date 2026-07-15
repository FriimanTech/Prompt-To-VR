#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>

#include "hud_wrist_menu_shared.h"

namespace ptv {

class HudWristMenu : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        (void)w; (void)e;
        state_ = HWState{};
        hw_parse_params(params, state_);
        // Start hidden
        state_.menu.open = false;
        state_.menu.visible = false;
        state_.menu.fading_in = false;
        state_.menu.alpha = 0.0f;
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        // Defensive: bail if entity invalid or no transform to drive visibility/placement
        if (!w.valid(e) || !w.has<CTransform>(e)) return;

        // Tick simple HUD timers and parse live-binding params if present
        hw_tick_hud(state_, dt);

        // Drive open/close transitions
        hw_tick_menu(state_, dt);

        // Apply visibility (scale as alpha, conservative to avoid occlusion)
        hw_apply_visibility(w, e, state_);

        // Place relative to player rig if found
        hw_place_menu(w, e, state_, dt);
    }

    void on_detach(World& w, std::uint32_t e) override {
        // Hide on detach
        if (w.valid(e) && w.has<CTransform>(e)) {
            auto& tr = w.get<CTransform>(e).t;
            tr.scale = 0.0f;
        }
    }

    std::string id() const override { return "generated.hud_wrist_menu"; }

private:
    HWState state_{};
};

PTV_REGISTER_MECHANIC("generated.hud_wrist_menu", HudWristMenu)

} // namespace ptv
