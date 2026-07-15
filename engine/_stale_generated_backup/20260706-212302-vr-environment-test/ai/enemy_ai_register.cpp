#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#include "enemy_ai_shared.h"

namespace ptv {

class EnemyAISystems : public IBehaviour {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        (void)params;
        if (!w.has<CTransform>(e)) return;
        // Cache archetype at attach time
        auto kind = ptv_ai_detect_archetype(w, e);
        auto& st = state_[e];
        st.archetype = kind;
        // Seed orbit angle a bit by entity id for variety
        st.orbit_angle = 0.3f * static_cast<float>((e % 31u));
        // Mark type tag for debug/readability
        switch (kind) {
            case EnemyArchetype::Drone:  ptv_ai_add_tag(w, e, "ai.drone"); break;
            case EnemyArchetype::Raider: ptv_ai_add_tag(w, e, "ai.raider"); break;
            case EnemyArchetype::Mech:   ptv_ai_add_tag(w, e, "ai.mech"); break;
            case EnemyArchetype::Fear:   ptv_ai_add_tag(w, e, "ai.fear"); break;
            default:                      ptv_ai_add_tag(w, e, "ai.unknown"); break;
        }
    }

    void on_tick(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        if (!w.has<CTransform>(e)) return;

        auto it = state_.find(e);
        if (it == state_.end()) {
            // Late-attach safety
            on_attach(w, e, nlohmann::json::object());
            it = state_.find(e);
            if (it == state_.end()) return;
        }
        auto& s = it->second;
        ptv_ai_step_timers(s.t, dt);

        // Update cached player position if visible
        if (auto p = ptv_ai_find_player(w)) s.last_player_pos = *p;

        // Archetype auto-detect fallback (mesh swapped, etc.)
        if (s.t.behaviour_swap_grace <= 0.0f) {
            auto now_kind = ptv_ai_detect_archetype(w, e);
            if (now_kind != s.archetype && now_kind != EnemyArchetype::Unknown) {
                s.archetype = now_kind;
                s.t.behaviour_swap_grace = 1.0f; // avoid thrashing for 1s
            }
        }

        // Simple CC reaction via tags: if entity has a tag that suggests stagger, pause
        if (w.has<CTag>(e)) {
            const auto& tg = w.get<CTag>(e);
            if (ptv_ai_has_tag(tg, "stagger") || ptv_ai_has_tag(tg, "caged")) {
                // Brief freeze, slowly clear local stagger timer
                s.t.stagger_time = std::max(s.t.stagger_time, 0.2f);
            }
        }
        if (s.t.stagger_time > 0.0f) return; // skip behaviours during stagger

        switch (s.archetype) {
            case EnemyArchetype::Drone:  ptv_ai_tick_drone(w, e, s, dt); break;
            case EnemyArchetype::Raider: ptv_ai_tick_raider(w, e, s, dt); break;
            case EnemyArchetype::Mech:   ptv_ai_tick_mech(w, e, s, dt); break;
            case EnemyArchetype::Fear:   ptv_ai_tick_fear(w, e, s, dt); break;
            default: /* Unknown: idle face player */ {
                auto& tr = w.get<CTransform>(e).t;
                ptv::Vec3 toP = s.last_player_pos - tr.position;
                float d2 = glm::dot(toP, toP);
                if (d2 > 0.0001f) {
                    toP = (1.0f / std::sqrt(d2)) * toP;
                    // Face-only: nudge forward slightly for readability
                    tr.position += 0.1f * dt * toP;
                }
            } break;
        }
    }

    void on_detach(World& w, std::uint32_t e) override {
        (void)w;
        state_.erase(e);
    }

    std::string id() const override { return "generated.enemy_ai_systems"; }

private:
    std::unordered_map<std::uint32_t, EnemyStateLocal> state_;
};

// ----------------- Shared helpers (defs) -----------------

std::optional<ptv::Vec3> ptv_ai_find_player(World& w) {
    auto& reg = w.registry();
    auto view = reg.view<ptv::CTransform, ptv::CTag>();
    for (auto ent : view) {
        auto eid = static_cast<std::uint32_t>(ent);
        if (!w.has<ptv::CTag>(eid)) continue;
        const auto& tag = w.get<ptv::CTag>(eid);
        if (ptv_ai_has_tag(tag, "player_avatar")) {
            return w.get<ptv::CTransform>(eid).t.position;
        }
    }
    return std::nullopt;
}

bool ptv_ai_has_tag(const ptv::CTag& tag, const std::string& s) {
    return std::find(tag.tags.begin(), tag.tags.end(), s) != tag.tags.end();
}

void ptv_ai_add_tag(World& w, std::uint32_t e, const std::string& s) {
    if (w.has<ptv::CTag>(e)) {
        auto t = w.get<ptv::CTag>(e);
        if (!ptv_ai_has_tag(t, s)) t.tags.push_back(s);
        w.emplace_or_replace<ptv::CTag>(e, t);
    } else {
        ptv::CTag t{}; t.tags.push_back(s);
        w.emplace_or_replace<ptv::CTag>(e, t);
    }
}

void ptv_ai_remove_tag(World& w, std::uint32_t e, const std::string& s) {
    if (!w.has<ptv::CTag>(e)) return;
    auto t = w.get<ptv::CTag>(e);
    t.tags.erase(std::remove(t.tags.begin(), t.tags.end(), s), t.tags.end());
    w.emplace_or_replace<ptv::CTag>(e, t);
}

EnemyArchetype ptv_ai_detect_archetype(World& w, std::uint32_t e) {
    if (!w.has<ptv::CMesh>(e)) {
        // Fallback by tags
        if (w.has<ptv::CTag>(e)) {
            const auto& t = w.get<ptv::CTag>(e);
            if (ptv_ai_has_tag(t, "flying")) return EnemyArchetype::Drone;
            if (ptv_ai_has_tag(t, "heavy")) return EnemyArchetype::Mech;
            if (ptv_ai_has_tag(t, "fear_energy") || ptv_ai_has_tag(t, "fear")) return EnemyArchetype::Fear;
        }
        return EnemyArchetype::Raider; // default bipedal
    }
    const auto& m = w.get<ptv::CMesh>(e);
    if (m.asset_id.find("robotic_drone_enemy") != std::string::npos) return EnemyArchetype::Drone;
    if (m.asset_id.find("alien_raider_enemy") != std::string::npos) return EnemyArchetype::Raider;
    if (m.asset_id.find("heavy_mech_enemy") != std::string::npos) return EnemyArchetype::Mech;
    if (m.asset_id.find("fear_construct_enemy") != std::string::npos) return EnemyArchetype::Fear;
    return EnemyArchetype::Unknown;
}

ptv::Vec3 ptv_ai_seek(const ptv::Vec3& from, const ptv::Vec3& to, float max_step) {
    ptv::Vec3 d = to - from;
    float d2 = glm::dot(d, d);
    if (d2 < 1e-6f) return from;
    float dlen = std::sqrt(d2);
    float step = std::min(max_step, dlen);
    return from + (step / dlen) * d;
}

ptv::Vec3 ptv_ai_orbit_point(const ptv::Vec3& center, float radius, float angle_rad, float y) {
    return ptv::Vec3{center.x + radius * std::cos(angle_rad), y, center.z + radius * std::sin(angle_rad)};
}

float ptv_ai_angle_wrap(float a) {
    const float TAU = 6.28318530718f;
    a = std::fmod(a, TAU);
    if (a < 0.0f) a += TAU;
    return a;
}

bool ptv_ai_in_cone(const ptv::Vec3& enemy_pos, const ptv::Vec3& enemy_fwd,
                    const ptv::Vec3& target_pos, float half_angle_deg, float max_range_m) {
    ptv::Vec3 v = target_pos - enemy_pos;
    float d2 = glm::dot(v, v);
    if (d2 > max_range_m * max_range_m) return false;
    float dlen = std::sqrt(std::max(d2, 1e-6f));
    ptv::Vec3 vn = (1.0f / dlen) * v;
    float cosang = glm::dot(vn, enemy_fwd);
    float coslimit = std::cos(half_angle_deg * 3.14159265f / 180.0f);
    return cosang >= coslimit;
}

PTV_REGISTER_BEHAVIOUR("generated.enemy_ai_systems", EnemyAISystems)

} // namespace ptv
