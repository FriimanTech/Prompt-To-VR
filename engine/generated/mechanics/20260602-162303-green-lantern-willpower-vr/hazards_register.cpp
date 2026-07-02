#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <optional>

#include "hazards_shared.h"

namespace ptv {

class EnvironmentalHazards : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        auto& rt = state_[e];
        rt.kind = infer_hazard_kind(w, e, params);
        // Configure from params when present
        if (rt.kind == HazardKind::Laser) {
            if (params.is_object()) {
                if (params.contains("laser_dir")) {
                    auto dv = j_vec3(params, "laser_dir");
                    if (dv) rt.laser.dir = safe_normalize(*dv);
                }
                rt.laser.max_range_m = params.value("laser_range_m", rt.laser.max_range_m);
                rt.laser.pulse_period_s = params.value("laser_pulse_period_s", rt.laser.pulse_period_s);
                if (params.contains("power_node_id")) rt.laser.power_node_id = params.at("power_node_id").get<std::string>();
            }
        } else if (rt.kind == HazardKind::Vent) {
            if (params.is_object()) {
                rt.vent.period_s = params.value("period_s", rt.vent.period_s);
                rt.vent.on_s = params.value("on_s", rt.vent.on_s);
                rt.vent.suction_strength = params.value("strength", rt.vent.suction_strength);
                rt.vent.fail_respawn = params.value("fail_respawn", rt.vent.fail_respawn);
            }
        } else if (rt.kind == HazardKind::DebrisSpawner) {
            if (params.is_object()) {
                rt.debris.interval_s = params.value("interval_s", rt.debris.interval_s);
                rt.debris.warn_s = params.value("warn_s", rt.debris.warn_s);
                rt.debris.speed_mps = params.value("speed_mps", rt.debris.speed_mps);
                rt.debris.ttl_s = params.value("ttl_s", rt.debris.ttl_s);
            }
        } else if (rt.kind == HazardKind::YellowZone) {
            if (params.is_object()) {
                rt.yellow.drain_multiplier = params.value("drain_multiplier", rt.yellow.drain_multiplier);
                rt.yellow.tether_break_add = params.value("tether_break_add", rt.yellow.tether_break_add);
            }
        }
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        auto it = state_.find(e);
        if (it == state_.end()) return;

        // Read live params from this binding's entry (for trigger zones etc.)
        nlohmann::json live_params;
        if (w.has<CMechanicBinding>(e)) {
            const auto& mb = w.get<CMechanicBinding>(e);
            for (const auto& entry : mb.entries) {
                if (entry.id == id()) {
                    live_params = nlohmann::json::parse(entry.params_json, nullptr, false);
                    break;
                }
            }
        }

        switch (it->second.kind) {
            case HazardKind::Laser:   update_laser(w, e, dt, it->second, live_params); break;
            case HazardKind::Vent:    update_vent(w, e, dt, it->second, live_params); break;
            case HazardKind::DebrisSpawner: update_debris(w, e, dt, it->second, live_params); break;
            case HazardKind::YellowZone: update_yellow(w, e, dt, it->second, live_params); break;
            default: break;
        }
    }

    void on_detach(World& w, std::uint32_t e) override {
        state_.erase(e);
    }

    std::string id() const override { return "generated.environmental_hazards"; }

private:
    std::unordered_map<std::uint32_t, HazardRuntime> state_;

    static ptv::Vec3 forward_from_rot(const ptv::Quat& q) {
        // Assuming engine Quat is w,x,y,z
        // Forward vector of Transform (0,0,1) rotated by q
        // Minimal, not allocating matrices
        float w_ = q.w, x = q.x, y = q.y, z = q.z;
        // Rotate vector (0,0,1)
        float vx = 2.0f*(x*z + w_*y);
        float vy = 2.0f*(y*z - w_*x);
        float vz = 1.0f - 2.0f*(x*x + y*y);
        return ptv::Vec3{vx, vy, vz};
    }

    void update_laser(World& w, std::uint32_t e, float dt, HazardRuntime& rt, const nlohmann::json& p) {
        if (!w.has<CTransform>(e)) return;
        auto& tr = w.get<CTransform>(e).t;
        // Base direction from params or entity rotation
        ptv::Vec3 dir = rt.laser.dir;
        if (dir.x==0 && dir.y==0 && dir.z==0) dir = safe_normalize(forward_from_rot(tr.rotation));
        // Optional reflection normal supplied by tests / barrier wall
        ptv::Vec3 out_dir = dir;
        if (p.is_object() && p.contains("reflection_normal")) {
            auto n = j_vec3(p, "reflection_normal");
            if (n) {
                auto nrm = safe_normalize(*n);
                out_dir = reflect(safe_normalize(dir), nrm);
            }
        }
        // Pulse VFX hint (emit lightweight tag)
        rt.laser.pulse_timer += dt;
        if (rt.laser.pulse_timer >= rt.laser.pulse_period_s) {
            rt.laser.pulse_timer = 0.0f;
            nlohmann::json jvfx = {
                {"id", "mining_laser_pulse"},
                {"pos", {{"x", tr.position.x},{"y", tr.position.y},{"z", tr.position.z}}},
                {"dir", {{"x", out_dir.x},{"y", out_dir.y},{"z", out_dir.z}}},
                {"ttl", 0.35f}
            };
            hazards_push_tag(w, e, "vfx.spawn:", jvfx);
        }
        // Power node alignment: if provided and roughly aimed within 8 degrees
        if (!rt.laser.powered_once && !rt.laser.power_node_id.empty()) {
            auto node_e = w.find(rt.laser.power_node_id);
            if (node_e != entt::null && w.has<CTransform>(static_cast<std::uint32_t>(node_e))) {
                auto& nt = w.get<CTransform>(static_cast<std::uint32_t>(node_e)).t;
                ptv::Vec3 to_node{ nt.position.x - tr.position.x, nt.position.y - tr.position.y, nt.position.z - tr.position.z };
                auto dn = safe_normalize(to_node);
                auto od = safe_normalize(out_dir);
                float dot = dn.x*od.x + dn.y*od.y + dn.z*od.z;
                if (dot > std::cos(8.0f * 3.14159265f/180.0f)) {
                    // Emit objective progress via Mission System contract
                    nlohmann::json jo = {
                        {"id", "laser_power_node"},
                        {"amount", 1.0f},
                        {"source", "mining_laser"}
                    };
                    hazards_push_tag(w, e, kObjectiveProgress, jo);
                    rt.laser.powered_once = true;
                    hazards_push_tag(w, e, "ui.toast:", nlohmann::json{{"text","Power node energized"},{"ttl",1.5f}});
                }
            }
        }
    }

    void update_vent(World& w, std::uint32_t e, float dt, HazardRuntime& rt, const nlohmann::json& p) {
        if (!w.has<CTransform>(e)) return;
        auto& tr = w.get<CTransform>(e).t;
        rt.vent.t += dt;
        float phase = std::fmod(rt.vent.t, std::max(0.001f, rt.vent.period_s));
        bool vent_on = (phase < rt.vent.on_s);

        // If trigger system supplies inside flag, honor it
        bool inside = p.is_object() ? p.value("inside", false) : false;

        if (vent_on) {
            // Pull toward vent center
            nlohmann::json jf = {
                {"dir", {{"x", -tr.position.x},{"y", -tr.position.y},{"z", -tr.position.z}}}, // world to origin-ish; scenes place vents at breach walls, acceptable heuristic
                {"strength", rt.vent.suction_strength},
                {"ttl", 0.2f}
            };
            hazards_push_tag(w, e, kHazardForce, jf);
            // Telegraph
            hazards_push_tag(w, e, "vfx.toggle:", nlohmann::json{{"id","vent_suction"},{"state","on"},{"ttl", rt.vent.on_s}});
        }
        if (!vent_on) {
            hazards_push_tag(w, e, "vfx.toggle:", nlohmann::json{{"id","vent_suction"},{"state","off"},{"ttl", 0.1f}});
        }
        if (inside && p.is_object() && p.value("fail_volume", false)) {
            // Request a respawn via a lightweight checkpoint hook
            hazards_push_tag(w, e, "checkpoint.respawn.request:", nlohmann::json{{"reason","decompression"}});
        }
    }

    void update_debris(World& w, std::uint32_t e, float dt, HazardRuntime& rt, const nlohmann::json& p) {
        if (!w.has<CTransform>(e)) return;
        auto& tr = w.get<CTransform>(e).t;
        rt.debris.t += dt;
        if (rt.debris.t >= rt.debris.interval_s) {
            rt.debris.t = 0.0f;
            // Warn
            nlohmann::json jw = {
                {"id", "debris_warning"},
                {"pos", {{"x", tr.position.x},{"y", tr.position.y+3.0f},{"z", tr.position.z}}},
                {"ttl", rt.debris.warn_s}
            };
            hazards_push_tag(w, e, "vfx.spawn:", jw);
            // Spawn a moment later implied by consumer; include sequence to vary
            ptv::Vec3 dir = p.is_object() && p.contains("spawn_dir") ? safe_normalize(*j_vec3(p, "spawn_dir")) : ptv::Vec3{0,-1,0};
            nlohmann::json js = {
                {"id", "asteroid_debris"},
                {"seq", rt.debris.sequence++},
                {"pos", {{"x", tr.position.x},{"y", tr.position.y+8.0f},{"z", tr.position.z}}},
                {"dir", {{"x", dir.x},{"y", dir.y},{"z", dir.z}}},
                {"speed", rt.debris.speed_mps},
                {"ttl", rt.debris.ttl_s}
            };
            hazards_push_tag(w, e, kSpawnerRequest, js);
        }
    }

    void update_yellow(World& w, std::uint32_t e, float, HazardRuntime& rt, const nlohmann::json& p) {
        bool inside = p.is_object() ? p.value("inside", false) : false;
        if (inside) {
            // Ring drain multiplier request
            hazards_push_tag(w, e, kRingDrainMultiplier, nlohmann::json{{"mult", rt.yellow.drain_multiplier},{"ttl", 0.2f}});
            // Control suppression
            hazards_push_tag(w, e, kControlSuppression, nlohmann::json{{"tether_break_add", rt.yellow.tether_break_add},{"ttl", 0.2f}});
            // HUD indicator amber suppression
            hazards_push_tag(w, e, kHudIndicator, nlohmann::json{{"id","suppression"},{"color","amber"},{"state","on"},{"ttl",0.3f}});
            if (!rt.yellow.active_last) {
                hazards_push_tag(w, e, "ui.toast:", nlohmann::json{{"text","Yellow energy suppresses constructs"},{"ttl",2.0f}});
            }
            rt.yellow.active_last = true;
        } else if (rt.yellow.active_last) {
            // Turn off indicator on exit
            hazards_push_tag(w, e, kHudIndicator, nlohmann::json{{"id","suppression"},{"state","off"},{"ttl",0.3f}});
            rt.yellow.active_last = false;
        }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.environmental_hazards", ptv::EnvironmentalHazards)
