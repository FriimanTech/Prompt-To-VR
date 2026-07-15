#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <algorithm>
#include <cmath>

#include "civilian_rescue_shared.h"

namespace ptv {

class CivilianRescueMechanic : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t self, const nlohmann::json& params) override;
    void on_update(World& w, std::uint32_t self, float dt) override;
    void on_detach(World& w, std::uint32_t self) override;
    std::string id() const override { return "generated.civilian_rescue_mechanic"; }

private:
    void refresh_entity_sets(World& w);
    int  pick_nearest_civilian_to_player(World& w) const;
    bool civilian_blocked_by_debris(World& w, std::uint32_t civ) const;
    void step_follow(World& w, int civ_index, float dt);
    void try_board_nearby_pod(World& w, int civ_index);
    void ensure_state_component(World& w, std::uint32_t self);
    void ensure_rescued_tag(World& w, std::uint32_t civ_entity);

private:
    // Cached ids
    std::vector<std::uint32_t> civilians_;
    std::vector<std::uint32_t> evac_pods_;

    // Per-civilian light state
    std::unordered_map<std::uint32_t, ShieldData> shields_;

    // Inputs/config
    std::string interact_button_{"A_right"};

    // Runtime
    float rescan_accum_{0.0f};
};

void CivilianRescueMechanic::on_attach(World& w, std::uint32_t self, const nlohmann::json& params) {
    interact_button_ = params.value("interact_button", std::string("A_right"));
    refresh_entity_sets(w);
    ensure_state_component(w, self);
    // initialize counts
    auto& st = w.get<CRescueState>(self);
    st.total = static_cast<int>(civilians_.size());
    st.rescued = 0;
    st.following = -1;
}

void CivilianRescueMechanic::on_update(World& w, std::uint32_t self, float dt) {
    rescan_accum_ += dt;
    if (rescan_accum_ > 1.0f) { // refresh sets periodically to capture spawned/removed entities
        rescan_accum_ = 0.0f;
        refresh_entity_sets(w);
        ensure_state_component(w, self);
        auto& st = w.get<CRescueState>(self);
        st.total = static_cast<int>(civilians_.size());
    }

    ensure_state_component(w, self);
    auto& st = w.get<CRescueState>(self);

    // Determine target civilian to follow: prefer an explicit interact signal via binding params, else proximity auto-pick
    int requested_index = -1;
    if (w.has<CMechanicBinding>(self)) {
        const auto& mb = w.get<CMechanicBinding>(self);
        for (const auto& e : mb.entries) {
            if (e.id == id()) {
                auto j = nlohmann::json::parse(e.params_json, nullptr, false);
                if (j.is_object() && j.value("interact_pressed", false)) {
                    requested_index = pick_nearest_civilian_to_player(w);
                }
                // Optional manual shield placement
                if (j.is_object() && j.value("place_shield_bubble", false)) {
                    int idx = pick_nearest_civilian_to_player(w);
                    if (idx >= 0 && idx < static_cast<int>(civilians_.size())) {
                        auto civ_e = civilians_[idx];
                        auto& sd = shields_[civ_e];
                        sd.active = true; sd.max_hp = j.value("shield_hp", 120.0f); sd.hp = sd.max_hp; sd.decay_per_s = j.value("shield_decay_per_s", 0.0f);
                    }
                }
            }
        }
    }

    if (requested_index < 0) {
        // proximity auto-pick if close and not already following
        if (st.following < 0) {
            int near_idx = pick_nearest_civilian_to_player(w);
            if (near_idx >= 0) {
                // require within 2.0m
                auto player_opt = find_player_avatar(w);
                if (player_opt.has_value() && w.has<CTransform>(*player_opt) && w.has<CTransform>(civilians_[near_idx])) {
                    const auto& ppos = w.get<CTransform>(*player_opt).t.position;
                    const auto& cpos = w.get<CTransform>(civilians_[near_idx]).t.position;
                    if (vec3_distance(ppos, cpos) <= 2.0f) {
                        requested_index = near_idx;
                    }
                }
            }
        }
    }

    // Start following
    if (requested_index >= 0) {
        st.following = requested_index;
    }

    // Auto-activate protective bubble if enemies nearby
    if (st.following >= 0 && st.following < static_cast<int>(civilians_.size())) {
        auto civ_e = civilians_[st.following];
        // Check for nearby enemies; if found and no active shield, spawn a bubble (logical only)
        bool enemy_near = false;
        auto& reg = w.registry();
        auto v = reg.view<CTag, CTransform>();
        if (w.has<CTransform>(civ_e)) {
            const auto& cpos = w.get<CTransform>(civ_e).t.position;
            for (auto e : v) {
                const auto& tg = v.get<CTag>(e);
                if (tag_list_contains(tg.tags, "enemy")) {
                    const auto& epos = v.get<CTransform>(e).t.position;
                    if (vec3_distance(cpos, epos) < 8.0f) { enemy_near = true; break; }
                }
            }
        }
        auto& sd = shields_[civ_e];
        if (enemy_near && !sd.active) { sd.active = true; sd.max_hp = 100.0f; sd.hp = sd.max_hp; sd.decay_per_s = 3.0f; }
        if (sd.active) {
            // passive decay to simulate absorbing fire; clamped to zero
            sd.hp = std::max(0.0f, sd.hp - sd.decay_per_s * dt);
            if (sd.hp <= 0.0f) sd.active = false;
        }
    }

    // Follow logic and evac check
    if (st.following >= 0 && st.following < static_cast<int>(civilians_.size())) {
        auto civ_e = civilians_[st.following];
        // Debris gate: if blocked, wait until debris moved/cleared
        if (!civilian_blocked_by_debris(w, civ_e)) {
            step_follow(w, st.following, dt);
            try_board_nearby_pod(w, st.following);
        }
    }
}

void CivilianRescueMechanic::on_detach(World& w, std::uint32_t self) {
    // Clear state if desired
    if (w.has<CRescueState>(self)) {
        w.remove<CRescueState>(self);
    }
    civilians_.clear();
    evac_pods_.clear();
    shields_.clear();
}

void CivilianRescueMechanic::refresh_entity_sets(World& w) {
    civilians_.clear();
    evac_pods_.clear();

    auto& reg = w.registry();
    auto vt = reg.view<CTag, CTransform>();
    for (auto e : vt) {
        const auto& tg = vt.get<CTag>(e);
        bool is_civ = tag_list_contains(tg.tags, "civilian") || tag_list_contains(tg.tags, "npc");
        bool is_pod = tag_list_contains(tg.tags, "evac") || tag_list_contains(tg.tags, "evac_pod");
        if (is_civ) civilians_.push_back(static_cast<std::uint32_t>(e));
        if (is_pod) evac_pods_.push_back(static_cast<std::uint32_t>(e));
    }
}

int CivilianRescueMechanic::pick_nearest_civilian_to_player(World& w) const {
    auto player_opt = find_player_avatar(w);
    if (!player_opt.has_value() || !w.has<CTransform>(*player_opt)) return -1;
    const auto& ppos = w.get<CTransform>(*player_opt).t.position;
    float best_d = 1e9f; int best_i = -1;
    for (int i = 0; i < static_cast<int>(civilians_.size()); ++i) {
        auto e = civilians_[i];
        if (!w.has<CTransform>(e)) continue;
        const auto& cpos = w.get<CTransform>(e).t.position;
        float d = vec3_distance(ppos, cpos);
        if (d < best_d) { best_d = d; best_i = i; }
    }
    return best_i;
}

bool CivilianRescueMechanic::civilian_blocked_by_debris(World& w, std::uint32_t civ) const {
    if (!w.has<CTransform>(civ)) return false;
    const auto& cpos = w.get<CTransform>(civ).t.position;
    // Consider any nearby heavy grabbable as debris pre-clear
    auto& reg = w.registry();
    auto v = reg.view<CTag, CTransform>();
    for (auto e : v) {
        const auto& tg = v.get<CTag>(e);
        bool is_debris = tag_list_contains(tg.tags, "grabbable") && tag_list_contains(tg.tags, "heavy");
        if (!is_debris) continue;
        const auto& dpos = v.get<CTransform>(e).t.position;
        if (vec3_distance(cpos, dpos) < 1.5f) {
            return true; // still blocked
        }
    }
    return false; // cleared
}

void CivilianRescueMechanic::step_follow(World& w, int civ_index, float dt) {
    if (civ_index < 0 || civ_index >= static_cast<int>(civilians_.size())) return;
    auto civ_e = civilians_[civ_index];
    auto player_opt = find_player_avatar(w);
    if (!player_opt.has_value()) return;
    if (!w.has<CTransform>(*player_opt) || !w.has<CTransform>(civ_e)) return;

    const auto& ppos = w.get<CTransform>(*player_opt).t.position;
    auto& ctr = w.get<CTransform>(civ_e).t;

    // Maintain a safe following distance behind the player (2.0 m)
    const float desired = 2.0f;
    float d = vec3_distance(ctr.position, ppos);
    if (d > desired) {
        // simple seek
        Vec3 dir{ppos.x - ctr.position.x, ppos.y - ctr.position.y, ppos.z - ctr.position.z};
        float inv = 1.0f / std::max(d, 0.001f);
        dir.x *= inv; dir.y *= inv; dir.z *= inv;
        const float speed = 1.6f; // m/s walking
        ctr.position.x += dir.x * speed * dt;
        ctr.position.y += dir.y * speed * dt;
        ctr.position.z += dir.z * speed * dt;
    }

    // If an evac pod is closer than the player by a margin, bias towards it to encourage completion
    float best_pd = 1e9f; std::uint32_t best_pod = 0;
    for (auto p : evac_pods_) {
        if (!w.has<CTransform>(p)) continue;
        float pd = vec3_distance(ctr.position, w.get<CTransform>(p).t.position);
        if (pd < best_pd) { best_pd = pd; best_pod = p; }
    }
    if (best_pod != 0 && best_pd + 1.0f < d) {
        Vec3 dir{w.get<CTransform>(best_pod).t.position.x - ctr.position.x,
                 w.get<CTransform>(best_pod).t.position.y - ctr.position.y,
                 w.get<CTransform>(best_pod).t.position.z - ctr.position.z};
        float nd = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
        if (nd > 0.001f) { dir.x/=nd; dir.y/=nd; dir.z/=nd; }
        const float speed = 1.6f;
        ctr.position.x += dir.x * speed * dt * 0.5f; // gentle bias
        ctr.position.y += dir.y * speed * dt * 0.5f;
        ctr.position.z += dir.z * speed * dt * 0.5f;
    }
}

void CivilianRescueMechanic::try_board_nearby_pod(World& w, int civ_index) {
    if (civ_index < 0 || civ_index >= static_cast<int>(civilians_.size())) return;
    auto civ_e = civilians_[civ_index];
    if (!w.has<CTransform>(civ_e)) return;

    const auto& cpos = w.get<CTransform>(civ_e).t.position;
    std::uint32_t best_pod = 0; float best_d = 1e9f;
    for (auto p : evac_pods_) {
        if (!w.has<CTransform>(p)) continue;
        float d = vec3_distance(cpos, w.get<CTransform>(p).t.position);
        if (d < best_d) { best_d = d; best_pod = p; }
    }
    if (best_pod != 0 && best_d < 1.2f) {
        // Mark rescued and softly despawn by moving into pod volume
        auto& ctr = w.get<CTransform>(civ_e).t;
        ctr.position = w.get<CTransform>(best_pod).t.position;
        ensure_rescued_tag(w, civ_e);

        // Bump counters on the controller that owns this mechanic
        // Find a controller with our component; prefer self if present
        // (callers attach this mechanic to a dedicated logic entity)
        // We increment rescued exactly once per boarding.
        // After tagging, remove from civilians_ list so we don't recount.
        for (auto it = civilians_.begin(); it != civilians_.end(); ++it) {
            if (*it == civ_e) { civilians_.erase(it); break; }
        }

        // Update any entity in world that holds CRescueState (typically the controller this runs on)
        auto& reg = w.registry();
        auto vs = reg.view<CRescueState>();
        for (auto e : vs) {
            auto& st = vs.get<CRescueState>(e);
            st.rescued += 1;
            // Clear following if we just boarded that one
            if (st.following >= static_cast<int>(civilians_.size())) st.following = -1;
        }
    }
}

void CivilianRescueMechanic::ensure_state_component(World& w, std::uint32_t self) {
    if (!w.has<CRescueState>(self)) {
        w.emplace_or_replace<CRescueState>(self, CRescueState{});
    }
}

void CivilianRescueMechanic::ensure_rescued_tag(World& w, std::uint32_t civ_entity) {
    if (!w.has<CTag>(civ_entity)) { w.emplace_or_replace<CTag>(civ_entity, CTag{}); }
    auto& t = w.get<CTag>(civ_entity);
    if (!tag_list_contains(t.tags, "rescued")) t.tags.push_back("rescued");
}

PTV_REGISTER_MECHANIC("generated.civilian_rescue_mechanic", CivilianRescueMechanic)

} // namespace ptv
