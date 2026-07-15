#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

// This mechanic emulates trigger/input/UI/scene-transition services using params and tags.
// Test harness can drive runtime via CMechanicBinding.params_json on this entity:
// {
//   "inside_trigger": true/false,        // optional override for proximity check
//   "cancel_pressed": true/false,        // one-frame or latched cancel
//   "countdown_s": 2.5,                 // optional override
//   "target_scene_id": "alien_city"     // optional override
// }
// Effects + transition are signaled via CTag on the portal entity:
//   - "hud:countdown:3", "hud:countdown:2", "hud:countdown:1"
//   - "vfx:skybox_space_nebula_fx"
//   - "warp_fx:emerald"
//   - "request_scene:<scene_id>" (emulates CSceneTransitionRequest)
//   - "transition:done"

namespace ptv {

namespace {
    enum class PortalState : std::uint8_t { Idle, Countdown, Warping };

    struct PortalRuntime {
        PortalState state{PortalState::Idle};
        float countdown_total{2.5f};
        float countdown_left{0.0f};
        float warp_time{0.0f};
        float warp_duration{6.0f};
        float trigger_radius_m{1.8f};
        float hover_alt_min_m{1.6f};
        float hover_alt_max_m{3.5f};
        std::string cancel_button{"B_right"};
        std::string target_scene_id{"alien_city"};
        bool player_was_inside{false};
        bool requested_transition{false};
    };

    // Helpers -----------------------------------------------------------------
    static bool has_tag(const CTag& tag, const std::string& needle) {
        return std::find(tag.tags.begin(), tag.tags.end(), needle) != tag.tags.end();
    }

    static void set_tags(World& w, std::uint32_t e, const std::vector<std::string>& tags) {
        CTag t{};
        t.tags = tags;
        w.emplace_or_replace<CTag>(e, std::move(t));
    }

    static void add_or_replace_tag(World& w, std::uint32_t e, const std::string& key, const std::string& val = "") {
        std::vector<std::string> tags;
        if (w.has<CTag>(e)) {
            tags = w.get<CTag>(e).tags;
        }
        // Remove any previous kv with same key prefix
        const std::string prefix = key + ":";
        tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const std::string& s){ return s == key || s.rfind(prefix, 0) == 0; }), tags.end());
        if (!val.empty()) tags.emplace_back(prefix + val); else tags.emplace_back(key);
        w.emplace_or_replace<CTag>(e, CTag{tags});
    }

    static bool parse_binding_flag(World& w, std::uint32_t e, const std::string& mech_id,
                                   const char* flag_key, bool def_val) {
        if (!w.has<CMechanicBinding>(e)) return def_val;
        const auto& mb = w.get<CMechanicBinding>(e);
        for (const auto& entry : mb.entries) {
            if (entry.id == mech_id || entry.builtin_id == mech_id) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object() && j.contains(flag_key)) {
                    try { return j.at(flag_key).get<bool>(); } catch (...) { return def_val; }
                }
            }
        }
        return def_val;
    }

    static float parse_binding_float(World& w, std::uint32_t e, const std::string& mech_id,
                                     const char* key, float def_val) {
        if (!w.has<CMechanicBinding>(e)) return def_val;
        const auto& mb = w.get<CMechanicBinding>(e);
        for (const auto& entry : mb.entries) {
            if (entry.id == mech_id || entry.builtin_id == mech_id) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object() && j.contains(key)) {
                    try { return j.at(key).get<float>(); } catch (...) { return def_val; }
                }
            }
        }
        return def_val;
    }

    static std::string parse_binding_string(World& w, std::uint32_t e, const std::string& mech_id,
                                            const char* key, const std::string& def_val) {
        if (!w.has<CMechanicBinding>(e)) return def_val;
        const auto& mb = w.get<CMechanicBinding>(e);
        for (const auto& entry : mb.entries) {
            if (entry.id == mech_id || entry.builtin_id == mech_id) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object() && j.contains(key)) {
                    try { return j.at(key).get<std::string>(); } catch (...) { return def_val; }
                }
            }
        }
        return def_val;
    }

    static bool find_player_avatar(World& w, std::uint32_t& out_entity) {
        auto& reg = w.registry();
        auto view = reg.view<CTag, CTransform>();
        for (auto e : view) {
            auto& tag = view.get<CTag>(e);
            if (std::find(tag.tags.begin(), tag.tags.end(), std::string("player_avatar")) != tag.tags.end()) {
                out_entity = static_cast<std::uint32_t>(e);
                return true;
            }
        }
        return false;
    }

    static float dist3(const Vec3& a, const Vec3& b) {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float dz = b.z - a.z;
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    }

    static void update_hud_countdown(World& w, std::uint32_t e, float seconds_left) {
        int sec = static_cast<int>(std::ceil(seconds_left));
        sec = std::clamp(sec, 0, 9);
        std::vector<std::string> tags;
        if (w.has<CTag>(e)) tags = w.get<CTag>(e).tags;
        // Remove previous hud:countdown:*
        tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& s){ return s.rfind("hud:countdown:", 0) == 0; }), tags.end());
        if (sec > 0) tags.emplace_back(std::string("hud:countdown:") + std::to_string(sec));
        // Ensure ambient VFX tag present during countdown
        if (std::find(tags.begin(), tags.end(), std::string("vfx:skybox_space_nebula_fx")) == tags.end())
            tags.emplace_back("vfx:skybox_space_nebula_fx");
        w.emplace_or_replace<CTag>(e, CTag{tags});
    }
}

class PortalTravelMechanic : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world; (void)entity;
        state_ = PortalRuntime{};
        state_.countdown_total = params.value("countdown_s", 2.5f);
        state_.cancel_button   = params.value("cancel_button", std::string("B_right"));
        state_.target_scene_id = params.value("target_scene_id", std::string("alien_city"));
        state_.countdown_left  = 0.0f;
        state_.warp_time       = 0.0f;
        state_.requested_transition = false;
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;
        if (!world.has<CTransform>(entity)) return;

        // Live overrides from binding (test harness can edit per-frame)
        const std::string mech_id = id();
        const bool inside_override = parse_binding_flag(world, entity, mech_id.c_str(), "inside_trigger", false);
        const bool cancel_pressed  = parse_binding_flag(world, entity, mech_id.c_str(), "cancel_pressed", false);
        const float cd_override    = parse_binding_float(world, entity, mech_id.c_str(), "countdown_s", state_.countdown_total);
        const std::string tgt_over = parse_binding_string(world, entity, mech_id.c_str(), "target_scene_id", state_.target_scene_id);
        state_.countdown_total = cd_override > 0.1f ? cd_override : state_.countdown_total;
        state_.target_scene_id = !tgt_over.empty() ? tgt_over : state_.target_scene_id;

        bool player_inside = inside_override;
        // Proximity-based trigger when no explicit override
        if (!player_inside) {
            std::uint32_t player{};
            if (find_player_avatar(world, player) && world.has<CTransform>(player)) {
                const auto& ppos = world.get<CTransform>(player).t.position;
                const auto& zpos = world.get<CTransform>(entity).t.position;
                const float d = dist3(ppos, zpos);
                const bool in_radius = d <= state_.trigger_radius_m;
                const bool at_hover_alt = (ppos.y >= state_.hover_alt_min_m && ppos.y <= state_.hover_alt_max_m);
                player_inside = in_radius || at_hover_alt; // alternate hover-start allowed
            }
        }

        switch (state_.state) {
            case PortalState::Idle: {
                if (player_inside && !state_.player_was_inside) {
                    // Begin countdown
                    state_.state = PortalState::Countdown;
                    state_.countdown_left = state_.countdown_total;
                    update_hud_countdown(world, entity, state_.countdown_left);
                }
                break;
            }
            case PortalState::Countdown: {
                // Cancel path
                if (cancel_pressed) {
                    state_.state = PortalState::Idle;
                    state_.countdown_left = 0.0f;
                    // Clear countdown HUD tags but keep other tags
                    if (world.has<CTag>(entity)) {
                        auto tags = world.get<CTag>(entity).tags;
                        tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& s){ return s.rfind("hud:countdown:", 0) == 0; }), tags.end());
                        world.emplace_or_replace<CTag>(entity, CTag{tags});
                    }
                    break;
                }
                // If player left trigger before completion, pause/reset gracefully
                if (!player_inside) {
                    // Soft reset to Idle
                    state_.state = PortalState::Idle;
                    state_.countdown_left = 0.0f;
                    if (world.has<CTag>(entity)) {
                        auto tags = world.get<CTag>(entity).tags;
                        tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& s){ return s.rfind("hud:countdown:", 0) == 0; }), tags.end());
                        world.emplace_or_replace<CTag>(entity, CTag{tags});
                    }
                    break;
                }
                // Progress countdown
                state_.countdown_left = std::max(0.0f, state_.countdown_left - dt);
                update_hud_countdown(world, entity, state_.countdown_left);
                if (state_.countdown_left <= 0.0f) {
                    // Enter warp transition
                    state_.state = PortalState::Warping;
                    state_.warp_time = 0.0f;
                    // Signal emerald warp VFX and request scene load (emulated via tags)
                    add_or_replace_tag(world, entity, "warp_fx", "emerald");
                    add_or_replace_tag(world, entity, "request_scene", state_.target_scene_id);
                    state_.requested_transition = true;
                }
                break;
            }
            case PortalState::Warping: {
                state_.warp_time += dt;
                // Keep VFX tag alive during the warp window
                add_or_replace_tag(world, entity, "warp_fx", "emerald");
                if (state_.warp_time >= state_.warp_duration) {
                    // Fade-in complete
                    add_or_replace_tag(world, entity, "transition", "done");
                    // Clear HUD countdown tags
                    if (world.has<CTag>(entity)) {
                        auto tags = world.get<CTag>(entity).tags;
                        tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& s){ return s.rfind("hud:countdown:", 0) == 0; }), tags.end());
                        world.emplace_or_replace<CTag>(entity, CTag{tags});
                    }
                    state_ = PortalRuntime{}; // reset to fresh idle with defaults preserved? reapply key fields
                    state_.cancel_button   = "B_right";
                    state_.target_scene_id = state_.target_scene_id; // keep last target by default
                    state_.countdown_total = cd_override_safe_; // keep last known override
                }
                break;
            }
        }

        state_.player_was_inside = player_inside;
        // Cache most recent override so we can persist after reset
        cd_override_safe_ = state_.countdown_total;
    }

    void on_detach(World& world, std::uint32_t entity) override {
        // Cleanup any countdown HUD tag when mechanic detaches
        if (world.valid(entity) && world.has<CTag>(entity)) {
            auto tags = world.get<CTag>(entity).tags;
            tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& s){ return s.rfind("hud:countdown:", 0) == 0; }), tags.end());
            world.emplace_or_replace<CTag>(entity, CTag{tags});
        }
    }

    std::string id() const override { return "generated.portal_travel_mechanic"; }

private:
    PortalRuntime state_{};
    float cd_override_safe_{2.5f};
};

PTV_REGISTER_MECHANIC("generated.portal_travel_mechanic", PortalTravelMechanic)

} // namespace ptv
