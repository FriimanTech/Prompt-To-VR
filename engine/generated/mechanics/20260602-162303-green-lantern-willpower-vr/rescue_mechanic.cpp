#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <span>
#include <optional>
#include <variant>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>
#include <chrono>
#include <type_traits>
#include <numeric>

// Forward declarations for mission API (provided by dependency 'mission_system').
namespace ptv { namespace mission_api {
    void post_objective_progress(std::uint32_t objective_id, float delta);
    void set_score_delta(float delta);
    std::uint32_t current_objective_id();
    bool mission_in_progress();
    float mission_elapsed();
    int current_difficulty();
} }

namespace ptv {

// Helper: tag utilities
static inline bool tag_has(const CTag& t, std::string_view key) {
    for (const auto& s : t.tags) if (s == key) return true; return false;
}
static inline void tag_add_unique(CTag& t, const std::string& key) {
    if (std::find(t.tags.begin(), t.tags.end(), key) == t.tags.end()) t.tags.push_back(key);
}
static inline void tag_remove(CTag& t, std::string_view key) {
    auto it = std::remove_if(t.tags.begin(), t.tags.end(), [&](const std::string& s){return s==key;});
    if (it!=t.tags.end()) t.tags.erase(it, t.tags.end());
}
static inline void tag_replace_prefix(CTag& t, std::string_view prefix, const std::string& repl) {
    for (auto& s : t.tags) {
        if (s.rfind(prefix, 0) == 0) { s = repl; return; }
    }
    t.tags.push_back(repl);
}

// Light-weight steering helpers
static inline Vec3 vec_from_to(const Vec3& a, const Vec3& b) { return Vec3{b.x - a.x, b.y - a.y, b.z - a.z}; }
static inline float vec_len(const Vec3& v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
static inline Vec3 vec_norm_or_zero(const Vec3& v) {
    float L = vec_len(v); if (L <= 1e-4f) return Vec3{0,0,0}; return Vec3{v.x/L, v.y/L, v.z/L};
}
static inline float dot3(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

struct CivilianStateData {
    enum Phase { Idle=0, Shielded=1, Following=2, Rescued=3 };
    Phase phase{Idle};
    float shield_hp{0.0f};
    float shield_hp_max{50.0f};
    float shield_decay_dps{0.0f};
    float pulse_t{0.0f};
    std::uint32_t evac_target{0}; // entity id of evac zone (std::uint32_t ABI id)
    bool debris_in_path{false};
    bool registered{false};
};

class CivilianRescueInteractions : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)params;
        // Ensure a CTag exists for HUD hints and state markers.
        world.emplace_or_replace<CTag>(entity, CTag{});
        // Lazy world scan is done during first update.
        if (!initialized_) {
            cache_world_indices(world);
            initialized_ = true;
        }
        // If this entity is a civilian, seed its state entry.
        if (world.has<CTag>(entity)) {
            auto& tag = world.get<CTag>(entity);
            if (tag_has(tag, "civilian")) {
                auto& s = civilians_[entity];
                s.phase = CivilianStateData::Idle;
                s.shield_hp = 0.0f;
                s.shield_hp_max = 50.0f;
                s.shield_decay_dps = 0.0f;
                s.pulse_t = 0.0f;
                s.evac_target = pick_nearest_evac(world, entity);
                s.debris_in_path = false;
                s.registered = true;
                tag_add_unique(tag, "hud:rescue_prompt");
                if (s.evac_target!=0) tag_add_unique(tag, "hud:evac_beacon_hint");
                world.emplace_or_replace<CTag>(entity, tag);
            }
        }
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;
        if (!initialized_) cache_world_indices(world);

        // Read inputs from this entity's own binding entry (params_json) if present.
        bool interact_pressed = false;
        bool request_bubble = false;
        bool start_escort = false;
        bool stop_escort = false;
        float incoming_damage_dps = 0.0f;
        parse_runtime_params(world, entity, interact_pressed, request_bubble, start_escort, stop_escort, incoming_damage_dps);

        // If this entity is an evac zone/trigger, make it HUD-visible.
        if (world.has<CTag>(entity)) {
            auto& t = world.get<CTag>(entity);
            if (tag_has(t, "evac_point") || tag_has(t, "objective")) {
                tag_add_unique(t, std::string("hud:evac_beacon"));
                world.emplace_or_replace<CTag>(entity, t);
            }
        }

        // Civilian logic if bound entity is a civilian.
        if (!world.has<CTag>(entity)) return;
        auto& tags = world.get<CTag>(entity);
        if (!tag_has(tags, "civilian")) return;

        auto it = civilians_.find(entity);
        if (it == civilians_.end()) return; // not registered yet
        CivilianStateData& st = it->second;

        // Find player reference position for prompts/escort.
        Vec3 player_pos{}; bool have_player=false;
        std::uint32_t player_entity = find_player_body(world);
        if (player_entity!=0 && world.has<CTransform>(player_entity)) {
            player_pos = world.get<CTransform>(player_entity).t.position; have_player=true;
        }

        // Optional hazard proximity auto-protect.
        bool hazard_close = is_hazard_nearby(world, entity, 4.0f);
        if (hazard_close) request_bubble = true; // auto-suggest bubble under threat

        // Apply bubble on request.
        if (request_bubble) {
            if (st.phase != CivilianStateData::Shielded) {
                st.phase = CivilianStateData::Shielded;
                st.shield_hp_max = 60.0f; // a bit more than default, short-duration protection
                st.shield_hp = st.shield_hp_max;
                st.shield_decay_dps = 0.0f;
                ensure_hud_tag(world, entity, "hud:shield_bubble_active");
            }
        }

        // Escort toggles
        float interact_dist_m = 2.0f;
        bool player_near = have_player && civilian_within_dist(world, entity, player_pos, interact_dist_m);
        if (interact_pressed && player_near) {
            if (stop_escort) {
                st.phase = CivilianStateData::Idle;
                tag_remove_if_exists(world, entity, "escort_follow");
            } else if (start_escort || st.phase==CivilianStateData::Idle) {
                st.phase = CivilianStateData::Following;
                tag_add_if_missing(world, entity, "escort_follow");
                // Re-pick nearest evac in case zones moved.
                st.evac_target = pick_nearest_evac(world, entity);
            }
        }

        // Simulate shield drain if under damage.
        if (st.phase==CivilianStateData::Shielded) {
            float dps = incoming_damage_dps;
            if (hazard_close) dps = std::max(dps, 5.0f); // ambient hazard drip
            st.shield_hp = std::max(0.0f, st.shield_hp - dps * dt);
            st.pulse_t += dt;
            // Visual hint: small scale pulse while shielded
            if (world.has<CTransform>(entity)) {
                auto& tr = world.get<CTransform>(entity).t;
                float base = 1.0f;
                tr.scale = base + 0.03f * std::sin(6.2831853f * 1.5f * st.pulse_t);
            }
            // HUD numeric: encode in a tag for HUD layer to render (best-effort)
            if (world.has<CTag>(entity)) {
                auto& tg = world.get<CTag>(entity);
                int hp_pct = (st.shield_hp_max>0.0f) ? int(std::round(100.0f * st.shield_hp / st.shield_hp_max)) : 0;
                tag_replace_prefix(tg, "hud:shield_hp:", std::string("hud:shield_hp:")+std::to_string(hp_pct));
                if (st.shield_hp<=0.0f) {
                    tag_remove(tg, "hud:shield_bubble_active");
                    st.phase = CivilianStateData::Idle; // bubble expired
                }
                world.emplace_or_replace<CTag>(entity, tg);
            }
        }

        // Escort movement and debris check
        if (st.phase==CivilianStateData::Following) {
            // Simple follow target: stay near player en route to evac, or directly head to evac if near it.
            Vec3 cur{}; bool have_cur=false;
            if (world.has<CTransform>(entity)) { cur = world.get<CTransform>(entity).t.position; have_cur=true; }
            std::uint32_t evac = st.evac_target;
            Vec3 evac_pos{}; bool have_evac=false;
            if (evac!=0 && world.valid(evac) && world.has<CTransform>(evac)) { evac_pos = world.get<CTransform>(evac).t.position; have_evac=true; }

            if (have_cur) {
                float speed = 1.6f; // m/s walking pace
                Vec3 target = cur;
                if (have_evac && vec_len(vec_from_to(cur, evac_pos)) < 6.0f) {
                    target = evac_pos;
                } else if (have_player) {
                    // orbit behind player a bit to avoid bumping
                    Vec3 to_player = vec_from_to(cur, player_pos);
                    float d = vec_len(to_player);
                    Vec3 dir = vec_norm_or_zero(to_player);
                    target = (d>1.0f) ? Vec3{player_pos.x - dir.x*1.0f, player_pos.y, player_pos.z - dir.z*1.0f} : cur;
                }
                Vec3 step = vec_norm_or_zero(vec_from_to(cur, target));
                if (world.has<CTransform>(entity)) {
                    auto& tr = world.get<CTransform>(entity).t;
                    tr.position.x += step.x * speed * dt;
                    tr.position.y += step.y * speed * dt;
                    tr.position.z += step.z * speed * dt;
                }

                // Debris prompt along path
                st.debris_in_path = detect_debris_on_path(world, cur, target, 1.0f);
                if (st.debris_in_path) ensure_hud_tag(world, entity, "hud:use_grabber");
                else remove_hud_tag(world, entity, "hud:use_grabber");
            }
        }

        // Arrival check: reaching evac zone grants score and completes this civilian.
        if (st.phase==CivilianStateData::Following || st.phase==CivilianStateData::Idle) {
            if (st.evac_target!=0 && world.valid(st.evac_target) && world.has<CTransform>(st.evac_target) && world.has<CTransform>(entity)) {
                auto& cpos = world.get<CTransform>(entity).t.position;
                auto& epos = world.get<CTransform>(st.evac_target).t.position;
                float d = vec_len(vec_from_to(cpos, epos));
                if (d < 1.5f) {
                    st.phase = CivilianStateData::Rescued;
                    // Score and objective progress
                    if (mission_api::mission_in_progress()) {
                        mission_api::set_score_delta(100.0f);
                        mission_api::post_objective_progress(mission_api::current_objective_id(), 1.0f);
                    }
                    // Mark HUD and remove escort tag
                    remove_hud_tag(world, entity, "hud:rescue_prompt");
                    remove_hud_tag(world, entity, "hud:use_grabber");
                    tag_remove_if_exists(world, entity, "escort_follow");
                    ensure_hud_tag(world, entity, "hud:rescued");
                }
            }
        }
    }

    void on_detach(World& world, std::uint32_t entity) override {
        (void)world; (void)entity;
    }

    std::string id() const override { return "generated.civilian_rescue_interactions"; }

private:
    bool initialized_{false};
    std::unordered_map<std::uint32_t, CivilianStateData> civilians_;

    void cache_world_indices(World& world) {
        // Mark evac zones with HUD tag so the player can see them.
        auto& reg = world.registry();
        auto view = reg.view<CTag>();
        for (auto e : view) {
            std::uint32_t id = static_cast<std::uint32_t>(e);
            if (!world.has<CTag>(id)) continue; // ensure safe ABI path
            auto& t = world.get<CTag>(id);
            if (tag_has(t, "evac_point") || tag_has(t, "objective")) {
                tag_add_unique(t, std::string("hud:evac_beacon"));
                world.emplace_or_replace<CTag>(id, t);
            }
        }
    }

    static void ensure_hud_tag(World& w, std::uint32_t e, const std::string& tag) {
        if (!w.has<CTag>(e)) { w.emplace_or_replace<CTag>(e, CTag{}); }
        auto& t = w.get<CTag>(e);
        tag_add_unique(t, tag);
        w.emplace_or_replace<CTag>(e, t);
    }
    static void remove_hud_tag(World& w, std::uint32_t e, const std::string& tag) {
        if (!w.has<CTag>(e)) return;
        auto& t = w.get<CTag>(e);
        tag_remove(t, tag);
        w.emplace_or_replace<CTag>(e, t);
    }
    static void tag_add_if_missing(World& w, std::uint32_t e, const std::string& tag) {
        if (!w.has<CTag>(e)) { w.emplace_or_replace<CTag>(e, CTag{}); }
        auto& t = w.get<CTag>(e);
        tag_add_unique(t, tag);
        w.emplace_or_replace<CTag>(e, t);
    }
    static void tag_remove_if_exists(World& w, std::uint32_t e, const std::string& tag) {
        if (!w.has<CTag>(e)) return;
        auto& t = w.get<CTag>(e);
        tag_remove(t, tag);
        w.emplace_or_replace<CTag>(e, t);
    }

    std::uint32_t pick_nearest_evac(World& world, std::uint32_t from_entity) const {
        if (!world.has<CTransform>(from_entity)) return 0;
        const auto& pos = world.get<CTransform>(from_entity).t.position;
        float best_d2 = FLT_MAX; std::uint32_t best=0;
        auto& reg = world.registry();
        auto view = reg.view<CTag, CTransform>();
        for (auto e : view) {
            std::uint32_t id = static_cast<std::uint32_t>(e);
            if (!world.has<CTag>(id)) continue;
            auto& t = world.get<CTag>(id);
            if (!(tag_has(t, "evac_point") || tag_has(t, "objective"))) continue;
            const auto& p = world.get<CTransform>(id).t.position;
            float dx = p.x - pos.x, dy = p.y - pos.y, dz = p.z - pos.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best_d2) { best_d2 = d2; best = id; }
        }
        return best;
    }

    bool detect_debris_on_path(World& world, const Vec3& a, const Vec3& b, float radius) const {
        // Checks for any grabbable object near the segment [a,b].
        auto& reg = world.registry();
        auto view = reg.view<CTag, CTransform>();
        Vec3 ab = vec_from_to(a,b); float ab2 = std::max(1e-3f, dot3(ab,ab));
        for (auto e : view) {
            std::uint32_t id = static_cast<std::uint32_t>(e);
            if (!world.has<CTag>(id)) continue;
            auto& t = world.get<CTag>(id);
            if (!tag_has(t, "grabbable")) continue;
            const auto& p = world.get<CTransform>(id).t.position;
            // project point p onto segment
            Vec3 ap = vec_from_to(a,p);
            float tproj = std::clamp(dot3(ap, ab) / ab2, 0.0f, 1.0f);
            Vec3 closest{ a.x + ab.x*tproj, a.y + ab.y*tproj, a.z + ab.z*tproj };
            float d = vec_len(vec_from_to(closest, p));
            if (d < radius) return true;
        }
        return false;
    }

    bool is_hazard_nearby(World& world, std::uint32_t around_entity, float radius) const {
        if (!world.has<CTransform>(around_entity)) return false;
        const auto& pos = world.get<CTransform>(around_entity).t.position;
        auto& reg = world.registry();
        auto view = reg.view<CTag, CTransform>();
        for (auto e : view) {
            std::uint32_t id = static_cast<std::uint32_t>(e);
            if (!world.has<CTag>(id)) continue;
            auto& t = world.get<CTag>(id);
            if (!tag_has(t, "hazard")) continue;
            const auto& p = world.get<CTransform>(id).t.position;
            float dx = p.x - pos.x, dy = p.y - pos.y, dz = p.z - pos.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < radius*radius) return true;
        }
        return false;
    }

    bool civilian_within_dist(World& world, std::uint32_t civilian, const Vec3& other, float dist) const {
        if (!world.has<CTransform>(civilian)) return false;
        const auto& p = world.get<CTransform>(civilian).t.position;
        float dx = p.x - other.x, dy = p.y - other.y, dz = p.z - other.z;
        return (dx*dx+dy*dy+dz*dz) < dist*dist;
    }

    std::uint32_t find_player_body(World& world) const {
        // Find any entity tagged player_avatar; prefer one with CTransform.
        auto& reg = world.registry();
        auto view = reg.view<CTag, CTransform>();
        for (auto e : view) {
            std::uint32_t id = static_cast<std::uint32_t>(e);
            if (!world.has<CTag>(id)) continue;
            auto& t = world.get<CTag>(id);
            if (tag_has(t, "player_avatar")) return id;
        }
        return 0;
    }

    void parse_runtime_params(World& world, std::uint32_t entity,
                               bool& interact, bool& bubble, bool& start_escort,
                               bool& stop_escort, float& incoming_dps) const {
        interact = bubble = start_escort = stop_escort = false; incoming_dps = 0.0f;
        if (!world.has<CMechanicBinding>(entity)) return;
        const auto& bind = world.get<CMechanicBinding>(entity);
        for (const auto& entry : bind.entries) {
            // We only care about our own binding entry on this entity
            if (entry.id != id()) continue;
            auto j = nlohmann::json::parse(entry.params_json, nullptr, /*allow_exceptions=*/false);
            if (!j.is_object()) continue;
            interact = j.value("interact", false);
            bubble = j.value("apply_bubble", false);
            start_escort = j.value("start_escort", false);
            stop_escort = j.value("stop_escort", false);
            incoming_dps = j.value("incoming_damage_dps", 0.0f);
            break;
        }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.civilian_rescue_interactions", ptv::CivilianRescueInteractions)
