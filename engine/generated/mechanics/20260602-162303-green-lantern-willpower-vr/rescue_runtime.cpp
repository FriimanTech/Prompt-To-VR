#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <cmath>

#include "rescue_shared.h"

namespace ptv {

static float len(const Vec3& v){ return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
static Vec3 norm(const Vec3& v){ float l=len(v); return (l>1e-4f)?Vec3{v.x/l,v.y/l,v.z/l}:Vec3{0,0,0}; }
static float dot3(const Vec3& a,const Vec3& b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static Vec3 sub(const Vec3& a,const Vec3& b){ return Vec3{a.x-b.x,a.y-b.y,a.z-b.z}; }
static Vec3 add(const Vec3& a,const Vec3& b){ return Vec3{a.x+b.x,a.y+b.y,a.z+b.z}; }
static Vec3 mul(const Vec3& a,float s){ return Vec3{a.x*s,a.y*s,a.z*s}; }

bool CivilianRescueInteractions::has_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) return false;
    const auto& tags = w.get<CTag>(e).tags;
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

void CivilianRescueInteractions::push_tag(World& w, std::uint32_t e, const std::string& s) {
    if (!w.has<CTag>(e)) { w.emplace_or_replace<CTag>(e, CTag{}); }
    auto& tags = w.get<CTag>(e).tags;
    tags.emplace_back(s);
}

nlohmann::json CivilianRescueInteractions::read_binding_params(World& w, std::uint32_t e, const std::string& mech_id) {
    nlohmann::json out = nlohmann::json::object();
    if (!w.has<CMechanicBinding>(e)) return out;
    const auto& mb = w.get<CMechanicBinding>(e);
    for (const auto& entry : mb.entries) {
        if (entry.id == mech_id || entry.builtin_id == mech_id) {
            nlohmann::json j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (j.is_object()) return j;
        }
    }
    return out;
}

void CivilianRescueInteractions::classify_and_cache(World& w, std::uint32_t e) {
    auto& st = state_[e];
    if (st.initialized) return;
    st.initialized = true;

    bool civ = has_tag(w,e,"civilian") || has_tag(w,e,"rescue_target");
    bool evac = has_tag(w,e,"evac_point") || has_tag(w,e,"objective");

    st.is_civilian = civ;
    st.is_evac_zone = evac;

    if (st.is_evac_zone) {
        if (w.has<CTransform>(e)) {
            auto& tr = w.get<CTransform>(e).t;
            EvacZoneInfo z; z.entity = e; z.position = tr.position; z.radius_m = std::max(1.0f, tr.scale); // scale used as radius hint
            evac_zones_.push_back(z);
        }
    }
}

std::optional<EvacZoneInfo> CivilianRescueInteractions::nearest_evac(World& w, const ptv::Vec3& pos) const {
    float best = 1e9f; std::optional<EvacZoneInfo> out;
    for (const auto& z : evac_zones_) {
        Vec3 d = sub(z.position, pos);
        float L = len(d);
        if (L < best) { best = L; out = z; }
    }
    return out;
}

bool CivilianRescueInteractions::path_is_blocked_by_debris(World& w, const Vec3& a, const Vec3& b, float corridor_m, Vec3* out_push_dir) {
    auto& reg = w.registry();
    auto view = reg.view<CTag, CTransform, CPhysics>();
    Vec3 ab = sub(b,a); float ab_len = len(ab); Vec3 ab_n = (ab_len>1e-4f)?mul(ab, 1.0f/ab_len):Vec3{0,0,0};
    bool blocked=false; Vec3 push{0,0,0};
    for (auto ent : view) {
        std::uint32_t e = static_cast<std::uint32_t>(ent);
        if (!has_tag(w, e, "grabbable")) continue;
        const auto& phys = w.get<CPhysics>(e);
        if (phys.mass_kg < debris_block_mass_kg_) continue;
        const auto& tr = w.get<CTransform>(e).t;
        Vec3 ap = sub(tr.position, a);
        float t = dot3(ap, ab_n); t = std::clamp(t, 0.0f, ab_len);
        Vec3 closest = add(a, mul(ab_n, t));
        float d = len(sub(tr.position, closest));
        if (d < corridor_m) {
            blocked = true;
            // push direction approximate: from debris to path center outward
            Vec3 away = norm(sub(tr.position, closest));
            push = add(push, away);
        }
    }
    if (out_push_dir) *out_push_dir = norm(push);
    return blocked;
}

void CivilianRescueInteractions::try_start_escort(World& w, std::uint32_t e, const nlohmann::json& p) {
    auto it = state_.find(e); if (it==state_.end()) return; auto& st = it->second; if (!st.is_civilian || st.rescued) return;

    bool a_tap = p.value("a_tap", p.value("a_pressed", false));

    // Proximity to player avatar
    Vec3 civ_pos{0,0,0};
    if (!w.has<CTransform>(e)) return; civ_pos = w.get<CTransform>(e).t.position;

    float nearest_player = 1e9f;
    auto& reg = w.registry();
    auto pv = reg.view<CTag, CTransform>();
    for (auto ent : pv) {
        std::uint32_t pe = static_cast<std::uint32_t>(ent);
        if (!has_tag(w, pe, "player_avatar")) continue;
        const auto& pt = w.get<CTransform>(pe).t;
        nearest_player = std::min(nearest_player, len(sub(pt.position, civ_pos)));
    }

    if (a_tap && nearest_player <= interact_radius_m_) {
        st.escort_active = !st.escort_active;
        st.path_blocked = false;
        // Assign target evac
        auto nz = nearest_evac(w, civ_pos);
        if (nz) st.target_evac = nz->entity;

        // Show context toast
        nlohmann::json toast{
            {"id", "rescue.interact"},
            {"text", st.escort_active ? std::string("Escorting civilian. Use Utility > Shield Bubble for protection.") : std::string("Escort cancelled.")},
            {"ttl", 2.0}
        };
        push_tag(w, e, std::string("ui.toast:")+ toast.dump());

        if (st.escort_active) {
            // Hint to utility constructs to spawn a protective bubble on this civilian (soft contract; another system may listen)
            nlohmann::json bubble_req{{"target_entity", e},{"hp", bubble_default_hp_},{"ttl_s", bubble_default_ttl_s_}};
            push_tag(w, e, std::string("utility.bubble.request:") + bubble_req.dump());
            st.bubble_active = true; st.bubble_hp = bubble_default_hp_; st.bubble_ttl_s = bubble_default_ttl_s_;
        }
    }
}

void CivilianRescueInteractions::update_civilian(World& w, std::uint32_t e, float dt) {
    auto it = state_.find(e); if (it==state_.end()) return; auto& st = it->second; if (!st.is_civilian) return;
    if (!w.has<CTransform>(e)) return; auto& tr = w.get<CTransform>(e).t;

    // Bubble maintenance near hazards (approximate soak)
    if (st.bubble_active) {
        st.bubble_ttl_s -= dt;
        // Simple hazard proximity check: any entity with tag "hazard" within radius drains the bubble
        auto& reg = w.registry();
        auto hv = reg.view<CTag, CTransform>();
        float drain = 0.0f;
        for (auto ent : hv) {
            std::uint32_t he = static_cast<std::uint32_t>(ent);
            if (!has_tag(w, he, "hazard")) continue;
            const auto& ht = w.get<CTransform>(he).t;
            float d = len(sub(ht.position, tr.position));
            if (d < hazard_avoid_radius_m_ + 0.5f) drain += bubble_hazard_dps_ * dt;
        }
        if (drain > 0.0f) st.bubble_hp = std::max(0.0f, st.bubble_hp - drain);
        if (st.bubble_ttl_s <= 0.0f || st.bubble_hp <= 0.0f) {
            st.bubble_active = false;
            nlohmann::json toast{{"id","rescue.bubble.down"},{"text","Shield Bubble depleted! Reapply if needed."},{"ttl",2.0}};
            push_tag(w, e, std::string("ui.toast:")+toast.dump());
        }
    }

    if (!st.escort_active || st.rescued) return;

    // Determine target evac each frame (in case zones move)
    auto nz = nearest_evac(w, tr.position);
    if (nz) { st.target_evac = nz->entity; }
    if (!st.target_evac || !w.valid(st.target_evac) || !w.has<CTransform>(st.target_evac)) return;

    const auto& et = w.get<CTransform>(st.target_evac).t;

    // Debris blocking check along corridor
    Vec3 push_dir{0,0,0};
    bool blocked = path_is_blocked_by_debris(w, tr.position, et.position, debris_corridor_halfwidth_m_, &push_dir);
    st.path_blocked = blocked;

    if (blocked) {
        st.ui_hint_cooldown_s -= dt;
        if (st.ui_hint_cooldown_s <= 0.0f) {
            nlohmann::json toast{{"id","rescue.debris.block"},{"text","Path blocked! Use Utility > Grabber Hand to move debris."},{"ttl",2.5}};
            push_tag(w, e, std::string("ui.toast:")+toast.dump());
            st.ui_hint_cooldown_s = 3.0f;
        }
        // Nudge civilian to wait and step aside slightly
        Vec3 aside = mul(push_dir, 0.2f);
        tr.position = add(tr.position, mul(aside, dt));
        return;
    }

    // Hazard avoidance (simple steering)
    Vec3 avoid{0,0,0};
    auto& reg = w.registry();
    auto hv = reg.view<CTag, CTransform>();
    for (auto ent : hv) {
        std::uint32_t he = static_cast<std::uint32_t>(ent);
        if (!has_tag(w, he, "hazard")) continue;
        const auto& ht = w.get<CTransform>(he).t;
        Vec3 d = sub(tr.position, ht.position);
        float L = len(d);
        if (L < hazard_avoid_radius_m_ && L>1e-3f) {
            avoid = add(avoid, mul(norm(d), (hazard_avoid_radius_m_ - L)));
        }
    }

    // Desired movement towards evac
    Vec3 to_goal = sub(et.position, tr.position);
    float dist = len(to_goal);
    if (dist <= std::max(1.0f, et.scale)) {
        // Arrived
        st.escort_active = false;
        st.rescued = true;
        // Mission progress + VO bark via toast
        nlohmann::json prog{{"id","civilian_rescue"},{"delta",1}};
        push_tag(w, e, std::string("objective.progress:")+prog.dump());
        nlohmann::json toast{{"id","rescue.arrived"},{"text","Civilian evacuated!"},{"ttl",2.0}};
        push_tag(w, e, std::string("ui.toast:")+toast.dump());
        // Optional haptics pulse
        nlohmann::json hap{{"pattern_id","construct_form_heavy"},{"hand","both"},{"intensity_scale",0.5},{"duration_s",0.15}};
        push_tag(w, e, std::string("haptics.publish:")+hap.dump());
        return;
    }

    Vec3 dir = norm(to_goal);
    Vec3 vel = mul(norm(add(dir, avoid)), escort_speed_mps_);
    tr.position = add(tr.position, mul(vel, dt));
}

void CivilianRescueInteractions::update_evac_marker(World& w, std::uint32_t e, float dt) {
    (void)dt;
    // Keep cached position/radius up to date in case this marker moves or scales
    if (!w.has<CTransform>(e)) return;
    auto& tr = w.get<CTransform>(e).t;
    for (auto& z : evac_zones_) {
        if (z.entity == e) { z.position = tr.position; z.radius_m = std::max(1.0f, tr.scale); break; }
    }
}

void CivilianRescueInteractions::on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) {
    // Read optional tunables
    interact_radius_m_ = params.value("interact_radius_m", interact_radius_m_);
    escort_speed_mps_ = params.value("escort_speed_mps", escort_speed_mps_);
    hazard_avoid_radius_m_ = params.value("hazard_avoid_radius_m", hazard_avoid_radius_m_);
    debris_block_mass_kg_ = params.value("debris_block_mass_kg", debris_block_mass_kg_);
    debris_corridor_halfwidth_m_ = params.value("debris_corridor_halfwidth_m", debris_corridor_halfwidth_m_);
    bubble_default_hp_ = params.value("bubble_default_hp", bubble_default_hp_);
    bubble_default_ttl_s_ = params.value("bubble_default_ttl_s", bubble_default_ttl_s_);

    classify_and_cache(world, entity);

    // If attached to a civilian, register to mission system via objective tag once
    if (has_tag(world, entity, "civilian")) {
        nlohmann::json toast{{"id","rescue.hint.start"},{"text","A: Interact to start escort. Use Bubble to protect."},{"ttl",3.0}};
        push_tag(world, entity, std::string("ui.toast:")+toast.dump());
        // Optional: register an evac beacon visual via generic beacon request (soft contract)
        nlohmann::json beacon{{"kind","evac_marker"},{"ttl_s",30.0}};
        push_tag(world, entity, std::string("utility.beacon.request:")+beacon.dump());
    }
}

void CivilianRescueInteractions::on_update(World& world, std::uint32_t entity, float dt) {
    if (!world.valid(entity)) return;
    classify_and_cache(world, entity);

    // Read this binding's input-style params
    auto params = read_binding_params(world, entity, id());

    auto it = state_.find(entity);
    if (it != state_.end()) {
        if (it->second.is_civilian) {
            try_start_escort(world, entity, params);
            update_civilian(world, entity, dt);
            return;
        }
        if (it->second.is_evac_zone) {
            update_evac_marker(world, entity, dt);
            return;
        }
    }

    // Fallback: if bound to a generic trigger volume meant to represent evac, treat as evac zone
    if (has_tag(world, entity, "trigger") && (has_tag(world, entity, "objective") || has_tag(world, entity, "evac_point"))) {
        state_[entity].is_evac_zone = true; state_[entity].initialized = true; update_evac_marker(world, entity, dt);
    }
}

void CivilianRescueInteractions::on_detach(World& world, std::uint32_t entity) {
    (void)world;
    state_.erase(entity);
    // Compact evac list if needed
    evac_zones_.erase(std::remove_if(evac_zones_.begin(), evac_zones_.end(), [&](const EvacZoneInfo& z){return z.entity==entity;}), evac_zones_.end());
}

} // namespace ptv
