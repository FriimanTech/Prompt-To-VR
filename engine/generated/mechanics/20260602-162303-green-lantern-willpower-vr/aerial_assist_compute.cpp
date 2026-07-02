#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <string>
#include <optional>

#include "aerial_assist_shared.h"

namespace ptv {

// Contract (tag strings we emit). Exposed via exports in register file.
static constexpr const char* kTagHudLead = "hud.lead:";          // JSON: {"x":..,"y":..,"z":..,"ttl":0.2}
static constexpr const char* kTagAimAssist = "aim.assist:";      // JSON: {"dir":{"x":..,"y":..,"z":..},"strength":..,"ttl":0.05}
static constexpr const char* kTagUIPageRegister = "ui.page.register:"; // From HUD/Wrist dependency

// Local store singleton (lives in this translation unit)
static AerialAssistStore g_store;

// Read this binding's params safely (per-frame or on-attach)
static void read_params(const nlohmann::json& p, AerialAssistParams& out){
    out.enable = p.value("enable", true);
    out.lead_stiffness = p.value("lead_stiffness", 0.6f);
    out.magnetism_cone_deg = p.value("magnetism_cone_deg", 5.0f);
    out.magnetism_strength = p.value("magnetism_strength", 0.15f);
    out.altitude_nudge_step_m = p.value("altitude_nudge_step_m", 0.2f);
    // Optional override if caller provides projectile speed
    out.projectile_speed_mps = p.value("projectile_speed_mps", 20.0f);
}

// Find a plausible target in front of shooter entity: choose nearest entity carrying tag "enemy" or "target" within a forward-ish cone.
static TargetSample find_target(World& w, std::uint32_t shooter){
    TargetSample best{};
    if (!w.has<CTransform>(shooter)) return best;
    const auto& shTr = w.get<CTransform>(shooter).t;
    const ptv::Vec3 shooter_pos = shTr.position;

    // Best-effort forward. If rotation is identity, +Z is forward in scenes.
    // We avoid quaternion math dependency; for selection we compare by smallest angle to (target - shooter).
    const ptv::Vec3 nominal_fwd{0.0f, 0.0f, 1.0f};

    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform>();
    for (auto e : view) {
        if ((std::uint32_t)e == shooter) continue;
        const auto& tag = view.get<CTag>(e);
        bool is_target = false;
        for (const auto& s : tag.tags) {
            if (s == "enemy" || s == "target") { is_target = true; break; }
        }
        if (!is_target) continue;
        const auto& tr = view.get<CTransform>(e).t;
        const ptv::Vec3 toT = ptv::Vec3{tr.position.x - shooter_pos.x, tr.position.y - shooter_pos.y, tr.position.z - shooter_pos.z};
        float d = len(toT);
        if (d < 0.1f) continue;
        float cosA = dot(norm(toT), nominal_fwd);
        // Require facing-ish (> ~ -60 deg) so we can acquire even when not perfectly aligned during flight
        if (cosA < -0.5f) continue;
        if (d < best.dist) {
            best.dist = d;
            best.pos = tr.position;
            // Velocity heuristic: if target has CPhysics with kind=="dynamic" we could infer, but we only have CTransform. Approx as 0.
            best.vel = ptv::Vec3{0.0f,0.0f,0.0f};
            best.valid = true;
        }
    }
    return best;
}

// Push a tag line onto entity (ensures CTag exists)
static void push_tag(World& w, std::uint32_t e, const std::string& line){
    if (!w.has<CTag>(e)) {
        w.emplace_or_replace<CTag>(e, CTag{});
    }
    auto& tg = w.get<CTag>(e);
    tg.tags.push_back(line);
}

class AerialCombatAssist : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        auto& st = g_store.map[e];
        read_params(params, st.p);
        // Register Wrist Menu page (via dependency contract). One-time hint; menu system dedups by id.
        nlohmann::json page = {
            {"id", "combat_assist"}, {"title", "Combat Assist"}, {"order", 35}
        };
        push_tag(w, e, std::string(kTagUIPageRegister) + page.dump());
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        auto it = g_store.map.find(e);
        if (it == g_store.map.end()) return;
        auto& st = it->second;

        // Read latest per-frame params from our binding entry (engine test harness drives these)
        // Find our own binding params in CMechanicBinding by id match
        if (w.has<CMechanicBinding>(e)) {
            const auto& mb = w.get<CMechanicBinding>(e);
            for (const auto& entry : mb.entries) {
                if (entry.id == id()) {
                    auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                    if (j.is_object()) {
                        read_params(j, st.p);
                        // Edge buttons for altitude micro-nudge
                        bool up = j.value("rs_up_tap", false);
                        bool down = j.value("rs_down_tap", false);
                        if (up && !st.rs_up_last && w.has<CTransform>(e)) {
                            auto& tr = w.get<CTransform>(e).t;
                            tr.position.y += st.p.altitude_nudge_step_m;
                        }
                        if (down && !st.rs_down_last && w.has<CTransform>(e)) {
                            auto& tr = w.get<CTransform>(e).t;
                            tr.position.y -= st.p.altitude_nudge_step_m;
                        }
                        st.rs_up_last = up; st.rs_down_last = down;
                        // Left Trigger refine tightens cone and increases strength while held
                        bool lt = j.value("left_trigger", false);
                        st.lt_last = lt; // tracked for potential future edges
                    }
                    break;
                }
            }
        }

        if (!st.p.enable) return;
        if (!w.has<CTransform>(e)) return;
        const auto& tr = w.get<CTransform>(e).t;
        const ptv::Vec3 shooter_pos = tr.position;

        // Acquire and predict
        TargetSample tgt = find_target(w, e);
        if (!tgt.valid) return;

        ptv::Vec3 aim_dir_nominal = norm(ptv::Vec3{0.0f,0.0f,1.0f}); // +Z forward fallback
        ptv::Vec3 r = ptv::Vec3{tgt.pos.x - shooter_pos.x, tgt.pos.y - shooter_pos.y, tgt.pos.z - shooter_pos.z};
        float use_speed = st.p.projectile_speed_mps;
        auto t_hit = lead_time(r, tgt.vel, use_speed);
        ptv::Vec3 lead_point = tgt.pos;
        if (t_hit.has_value()) {
            float th = *t_hit;
            lead_point = ptv::Vec3{ tgt.pos.x + tgt.vel.x*th, tgt.pos.y + tgt.vel.y*th, tgt.pos.z + tgt.vel.z*th };
        }

        // Reticle stabilization (low-pass on presented anchor)
        if (!st.has_anchor) {
            st.filtered_anchor = lead_point; st.has_anchor = true;
        } else {
            float alpha = clamp(1.0f - std::exp(-dt / std::max(0.001f, st.p.lead_stiffness)), 0.0f, 1.0f);
            st.filtered_anchor = lerp(st.filtered_anchor, lead_point, alpha);
        }

        // Gentle magnetism within cone
        ptv::Vec3 raw_dir = norm(r); // what the player is roughly aiming towards (to target center)
        ptv::Vec3 desired_dir = norm(ptv::Vec3{ st.filtered_anchor.x - shooter_pos.x, st.filtered_anchor.y - shooter_pos.y, st.filtered_anchor.z - shooter_pos.z });
        float cosCone = cos_deg( (st.lt_last ? std::max(1.0f, st.p.magnetism_cone_deg*0.5f) : st.p.magnetism_cone_deg) );
        float cosA = dot(norm(raw_dir), norm(desired_dir));
        if (cosA >= cosCone) {
            float k = st.p.magnetism_strength * (st.lt_last ? 1.5f : 1.0f);
            k = clamp(k, 0.0f, 0.6f); // never overpower input
            ptv::Vec3 nudged = norm( lerp(raw_dir, desired_dir, k) );
            // Publish assist direction for projectile spawners that honor it
            nlohmann::json ja = {
                {"dir", {{"x", nudged.x},{"y", nudged.y},{"z", nudged.z}}},
                {"strength", k}, {"ttl", 0.05f}
            };
            push_tag(w, e, std::string(kTagAimAssist) + ja.dump());
        }

        // Publish HUD lead indicator world position (ring-style reticle renderer can consume this)
        nlohmann::json j = {{"x", st.filtered_anchor.x},{"y", st.filtered_anchor.y},{"z", st.filtered_anchor.z},{"ttl", 0.2f}};
        push_tag(w, e, std::string(kTagHudLead) + j.dump());
    }

    void on_detach(World& w, std::uint32_t e) override {
        g_store.map.erase(e);
    }

    std::string id() const override { return "generated.aerial_combat_assist"; }
};

} // namespace ptv

// Registration stays in separate translation unit as advised
