#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <cstring>

#include "portal_shared.h"

namespace ptv {

// Forward from helper unit
void maybe_emit_practice_spawn(World& world, std::uint32_t self, int max_alive, float& t_since_spawn, float cooldown_s);

class PortalTravel : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        // Register a small Wrist Menu page chip for status/debug.
        ensure_tags(world, entity);
        auto& tags = world.get<CTag>(entity).tags;
        tags.push_back(rt_.tag_ui_page_register + std::string("{\"id\":\"portal\",\"title\":\"Portal\",\"order\":12}"));

        // Initial config
        cfg_.practice_drones = params.value("practice_drones", true);
        cfg_.practice_max_alive = params.value("practice_max_alive", 3);
        cfg_.practice_spawn_cooldown_s = params.value("practice_spawn_cooldown_s", 2.0f);
        rt_.phase = PortalPhase::IdleAtLaunch;
        rt_.t_fade = 0.0f;
        rt_.t_since_spawn = 0.0f;
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        // Read per-frame inputs from our binding params (driven by tests):
        read_inputs_from_binding(world, entity);

        // Listen for mission_system handshake: portal.travel.request:{ mission_id, scene_id, mission_name }
        if (rt_.phase == PortalPhase::IdleAtLaunch) {
            consume_portal_request_if_any(world, entity);
        }

        switch (rt_.phase) {
            case PortalPhase::IdleAtLaunch: {
                // Visual hint when a mission is selected (armed state handled below)
                break;
            }
            case PortalPhase::ArmedAwaitConfirm: {
                // Show portal VFX hint + HUD toast once upon arming
                if (!armed_announced_) {
                    armed_announced_ = true;
                    show_portal_ready(world, entity);
                }
                if (edge_press(cfg_.a_pressed, rt_.a_was_held)) {
                    // Confirm travel
                    rt_.phase = PortalPhase::FadeOut;
                    rt_.t_fade = 0.0f;
                    publish_portal_activated(world, entity);
                    fade_out(world, entity, 0.35f);
                }
                break;
            }
            case PortalPhase::FadeOut: {
                rt_.t_fade += dt;
                if (rt_.t_fade >= 0.35f) {
                    rt_.phase = PortalPhase::LoadingCorridor;
                    request_scene_load(world, entity, "sector_travel_corridor");
                }
                break;
            }
            case PortalPhase::LoadingCorridor: {
                // Give a short grace, then assume corridor active
                corridor_gate_timer_ += dt;
                if (corridor_gate_timer_ > 0.25f) {
                    corridor_gate_timer_ = 0.0f;
                    rt_.phase = PortalPhase::InCorridor;
                    // Optional practice targets
                    if (cfg_.practice_drones) {
                        maybe_emit_practice_spawn(world, entity, cfg_.practice_max_alive, rt_.t_since_spawn, cfg_.practice_spawn_cooldown_s);
                    }
                }
                break;
            }
            case PortalPhase::InCorridor: {
                // Allow practice spawns on cadence
                if (cfg_.practice_drones) {
                    maybe_emit_practice_spawn(world, entity, cfg_.practice_max_alive, rt_.t_since_spawn, cfg_.practice_spawn_cooldown_s);
                }
                // Emergency cancel on B
                if (edge_press(cfg_.b_pressed, rt_.b_was_held)) {
                    rt_.phase = PortalPhase::Cancelling;
                    corridor_gate_timer_ = 0.0f;
                    toast(world, entity, "Travel cancelled — returning to Launch Platform");
                    request_scene_load(world, entity, "launch_platform");
                }
                // Auto-handoff could be triggered by test via a param flag; otherwise leave control here.
                if (handoff_requested_via_param_) {
                    rt_.phase = PortalPhase::HandoffToMission;
                    request_scene_load(world, entity, rt_.target ? rt_.target->scene_id : std::string("training_arena"));
                }
                break;
            }
            case PortalPhase::Cancelling: {
                // After scene request, quickly restore idle
                corridor_gate_timer_ += dt;
                if (corridor_gate_timer_ > 0.25f) {
                    reset_to_launch_idle();
                }
                break;
            }
            case PortalPhase::HandoffToMission: {
                // After requesting mission scene load, we can clear to idle for next cycle
                corridor_gate_timer_ += dt;
                if (corridor_gate_timer_ > 0.25f) {
                    reset_to_launch_idle();
                }
                break;
            }
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.portal_travel"; }

private:
    PortalConfig cfg_{};
    PortalRuntime rt_{};

    bool armed_announced_ = false;
    bool handoff_requested_via_param_ = false;
    float corridor_gate_timer_ = 0.0f;

    static void ensure_tags(World& w, std::uint32_t e) {
        if (!w.has<CTag>(e)) w.emplace_or_replace<CTag>(e, CTag{});
    }

    void toast(World& world, std::uint32_t e, const std::string& msg) {
        ensure_tags(world, e);
        world.get<CTag>(e).tags.push_back(rt_.tag_ui_toast + std::string("{\"text\":\"") + msg + std::string("\",\"ttl\":2.0}"));
    }

    void show_portal_ready(World& world, std::uint32_t e) {
        if (!rt_.target) return;
        ensure_tags(world, e);
        // Enable gate VFX preview and show mission name
        world.get<CTag>(e).tags.push_back(std::string("vfx.toggle:{\"id\":\"sector_travel_corridor_vfx\",\"enable\":true}"));
        toast(world, e, std::string("Ready: ") + rt_.target->mission_name + std::string(" — Press A to travel"));
    }

    void publish_portal_activated(World& world, std::uint32_t e) {
        if (!rt_.target) return;
        ensure_tags(world, e);
        nlohmann::json j{
            {"mission_id", rt_.target->mission_id},
            {"scene_id", rt_.target->scene_id}
        };
        world.get<CTag>(e).tags.push_back(rt_.tag_portal_activated + j.dump());
    }

    void fade_out(World& world, std::uint32_t e, float dur) {
        ensure_tags(world, e);
        world.get<CTag>(e).tags.push_back(std::string("mask.fade:{\"state\":\"out\",\"dur_s\":") + std::to_string(dur) + std::string("}"));
    }

    void request_scene_load(World& world, std::uint32_t e, const std::string& scene_id) {
        ensure_tags(world, e);
        nlohmann::json j{{"id", scene_id}};
        world.get<CTag>(e).tags.push_back(std::string("scene.load:") + j.dump());
    }

    bool edge_press(bool held_now, bool& held_prev) {
        bool edge = (held_now && !held_prev);
        held_prev = held_now;
        return edge;
    }

    void read_inputs_from_binding(World& world, std::uint32_t entity) {
        // Default fallbacks
        cfg_.a_pressed = false;
        cfg_.b_pressed = false;
        handoff_requested_via_param_ = false;

        // Pull our binding params_json (if present) from CMechanicBinding on this entity
        if (!world.has<CMechanicBinding>(entity)) return;
        auto& mb = world.get<CMechanicBinding>(entity);
        for (auto& entry : mb.entries) {
            if (entry.id == id()) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (!j.is_object()) continue;
                cfg_.a_pressed = j.value("a_pressed", false);
                cfg_.b_pressed = j.value("b_pressed", false);
                cfg_.practice_drones = j.value("practice_drones", cfg_.practice_drones);
                cfg_.practice_max_alive = j.value("practice_max_alive", cfg_.practice_max_alive);
                cfg_.practice_spawn_cooldown_s = j.value("practice_spawn_cooldown_s", cfg_.practice_spawn_cooldown_s);
                handoff_requested_via_param_ = j.value("handoff_now", false);
            }
        }
    }

    void consume_portal_request_if_any(World& world, std::uint32_t entity) {
        // Scan all entities for CTag strings starting with portal.travel.request:
        auto& reg = world.registry();
        auto view = reg.view<CTag>();
        for (auto e : view) {
            auto& tags = reg.get<CTag>(e).tags;
            for (auto& s : tags) {
                auto j = parse_payload_after_prefix(s, rt_.tag_portal_request);
                if (!j.is_object()) continue;
                std::string raw = j.dump();
                if (raw == rt_.last_request_payload) continue; // already consumed

                PortalTarget t{};
                t.mission_id = j.value("mission_id", std::string("unknown"));
                t.scene_id = j.value("scene_id", std::string("training_arena"));
                t.mission_name = j.value("mission_name", std::string("Mission"));
                rt_.target = t;
                rt_.last_request_payload = raw;
                rt_.phase = PortalPhase::ArmedAwaitConfirm;
                armed_announced_ = false; // allow one-time VFX + toast
                // Mirror a tiny confirmation toast at the portal gate
                toast(world, entity, std::string("Selected: ") + t.mission_name);
                return;
            }
        }
    }

    void reset_to_launch_idle() {
        rt_.phase = PortalPhase::IdleAtLaunch;
        rt_.target.reset();
        corridor_gate_timer_ = 0.0f;
        rt_.t_fade = 0.0f;
        rt_.t_since_spawn = 0.0f;
        armed_announced_ = false;
        // Keep last_request_payload so we don't immediately re-arm unless a new request arrives
    }
};

} // namespace ptv
