#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <cmath>
#include <algorithm>
#include <optional>

namespace ptv {

namespace mission_api {
    void post_objective_progress(std::uint32_t actor_entity, float amount);
    void set_score_delta(float amount);
    bool mission_in_progress();
}

struct HazardConfig {
    float radius{5.0f};
    float period_s{3.0f};
    float strength{1.0f};
    bool laser_reflectable{true};
    float laser_progress_rate{0.1f};
    float vent_burst_duty{0.4f};
    float vent_pull_mps{2.0f};
    float debris_interval_s{4.0f};
    float debris_telegraph_s{1.5f};
    float debris_hit_radius{1.0f};
};

static void parse_hazard_config(const nlohmann::json& params, HazardConfig& cfg) {
    if (!params.is_object()) return;
    cfg.radius = params.value("radius", cfg.radius);
    cfg.period_s = params.value("period_s", cfg.period_s);
    cfg.strength = params.value("strength", cfg.strength);
    cfg.laser_reflectable = params.value("laser_reflectable", cfg.laser_reflectable);
    cfg.laser_progress_rate = params.value("laser_progress_rate", cfg.laser_progress_rate);
    cfg.vent_burst_duty = params.value("vent_burst_duty", cfg.vent_burst_duty);
    cfg.vent_pull_mps = params.value("vent_pull_mps", cfg.vent_pull_mps);
    cfg.debris_interval_s = params.value("debris_interval_s", cfg.debris_interval_s);
    cfg.debris_telegraph_s = params.value("debris_telegraph_s", cfg.debris_telegraph_s);
    cfg.debris_hit_radius = params.value("debris_hit_radius", cfg.debris_hit_radius);
}

static bool entity_has_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) return false;
    const auto& tags = w.get<CTag>(e).tags;
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

static void ensure_tag(World& w, std::uint32_t e, const std::string& tag) {
    CTag ct;
    if (w.has<CTag>(e)) ct = w.get<CTag>(e);
    if (std::find(ct.tags.begin(), ct.tags.end(), tag) == ct.tags.end()) {
        ct.tags.push_back(tag);
        w.emplace_or_replace<CTag>(e, ct);
    }
}

static void remove_tag(World& w, std::uint32_t e, const std::string& tag) {
    if (!w.has<CTag>(e)) return;
    auto ct = w.get<CTag>(e);
    ct.tags.erase(std::remove(ct.tags.begin(), ct.tags.end(), tag), ct.tags.end());
    w.emplace_or_replace<CTag>(e, ct);
}

static std::optional<std::uint32_t> find_first_player(World& w) {
    auto& reg = w.registry();
    auto view = reg.view<CTag>();
    for (auto ent : view) {
        const auto& tags = reg.get<CTag>(ent).tags;
        if (std::find(tags.begin(), tags.end(), "player_avatar") != tags.end() ||
            std::find(tags.begin(), tags.end(), "player") != tags.end()) {
            return static_cast<std::uint32_t>(ent);
        }
    }
    return std::nullopt;
}

static float dist(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static void approach(float& current, float target, float speed, float dt) {
    current += (target - current) * std::min(1.0f, speed * dt);
}

static float segment_point_distance(const Vec3& a, const Vec3& b, const Vec3& p) {
    Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    Vec3 ap{p.x - a.x, p.y - a.y, p.z - a.z};
    float ab_dot = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    if (ab_dot < 1e-8f) return dist(a, p);
    float t = std::max(0.0f, std::min(1.0f, (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / ab_dot));
    Vec3 proj{a.x + t * ab.x, a.y + t * ab.y, a.z + t * ab.z};
    return dist(proj, p);
}

enum class HazardType { kUnknown, kLaser, kVent, kDebris, kYellow };

struct LaserState {
    float t{0.f};
    float progress{0.f};
    bool last_reflected{false};
};
struct VentState {
    float t{0.f};
};
struct DebrisState {
    float t{0.f};
    bool telegraph{false};
    float telegraph_t{0.f};
};
struct YellowState {
    float level{0.f};
    bool inside{false};
};

using StateVariant = std::variant<std::monostate, LaserState, VentState, DebrisState, YellowState>;

class EnvironmentalHazards : public IMechanic {
public:
    void on_attach(World& w, std::uint32_t e, const nlohmann::json& params) override {
        type_ = HazardType::kUnknown;
        if (entity_has_tag(w, e, "laser")) type_ = HazardType::kLaser;
        else if (entity_has_tag(w, e, "vent")) type_ = HazardType::kVent;
        else if (entity_has_tag(w, e, "yellow_zone")) type_ = HazardType::kYellow;
        else if (entity_has_tag(w, e, "hazard")) type_ = HazardType::kDebris;

        parse_hazard_config(params, cfg_);

        if (params.contains("debris")) {
            auto d = params.at("debris");
            if (d.contains("spawn_local")) debris_spawn_local_ = to_vec3(d.at("spawn_local"));
            if (d.contains("impact_local")) debris_impact_local_ = to_vec3(d.at("impact_local"));
        }

        switch (type_) {
            case HazardType::kLaser: state_ = LaserState{}; break;
            case HazardType::kVent: state_ = VentState{}; break;
            case HazardType::kDebris: state_ = DebrisState{}; break;
            case HazardType::kYellow: state_ = YellowState{}; break;
            default: state_ = std::monostate{}; break;
        }
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (!w.valid(e)) return;
        if (!w.has<CTransform>(e)) return;
        auto& tr = w.get<CTransform>(e).t;

        switch (type_) {
            case HazardType::kLaser: tick_laser(w, e, tr, dt); break;
            case HazardType::kVent: tick_vent(w, e, tr, dt); break;
            case HazardType::kDebris: tick_debris(w, e, tr, dt); break;
            case HazardType::kYellow: tick_yellow(w, e, tr, dt); break;
            default: break;
        }
    }

    void on_detach(World& w, std::uint32_t e) override {
        if (type_ == HazardType::kYellow) {
            if (auto pe = find_first_player(w)) {
                remove_tag(w, *pe, "yellow_suppression_active");
                remove_tag(w, *pe, "ring_drain_x1.5");
                remove_tag(w, *pe, "control_tether_unstable");
            }
        }
    }

    std::string id() const override { return "generated.environmental_hazards"; }

private:
    static Vec3 to_vec3(const nlohmann::json& j) {
        Vec3 v{0,0,0};
        if (j.is_array() && j.size()==3) { v.x=j[0].get<float>(); v.y=j[1].get<float>(); v.z=j[2].get<float>(); }
        else if (j.is_object()) {
            v.x = j.value("x", 0.0f); v.y = j.value("y", 0.0f); v.z = j.value("z", 0.0f);
        }
        return v;
    }

    void tick_laser(World& w, std::uint32_t e, Transform& tr, float dt) {
        auto& s = std::get<LaserState>(state_);
        s.t += dt;

        bool reflected = false;
        if (cfg_.laser_reflectable) {
            reflected = find_barrier_nearby(w, tr.position, 4.0f);
        }

        if (reflected) {
            s.progress += cfg_.laser_progress_rate * dt;
            if (mission_api::mission_in_progress()) {
                mission_api::post_objective_progress(e, s.progress);
            }
        }
        s.last_reflected = reflected;

        if (reflected) ensure_tag(w, e, "laser_reflected");
        else remove_tag(w, e, "laser_reflected");
    }

    bool find_barrier_nearby(World& w, const Vec3& pos, float r) {
        auto& reg = w.registry();
        auto view = reg.view<CTag, CTransform>();
        for (auto ent : view) {
            std::uint32_t x = static_cast<std::uint32_t>(ent);
            if (!entity_has_tag(w, x, "defense")) continue;
            if (entity_has_tag(w, x, "barrier_wall") || entity_has_tag(w, x, "dome_shield") || entity_has_tag(w, x, "round_shield")) {
                const auto& tp = w.get<CTransform>(x).t.position;
                if (dist(tp, pos) <= r) return true;
            }
        }
        return false;
    }

    void tick_vent(World& w, std::uint32_t e, Transform& tr, float dt) {
        auto& s = std::get<VentState>(state_);
        s.t += dt;
        auto pe = find_first_player(w);
        if (!pe) return;
        if (!w.has<CTransform>(*pe)) return;
        auto& ptr = w.get<CTransform>(*pe).t;
        float d = dist(ptr.position, tr.position);
        if (d > cfg_.radius) { remove_tag(w, *pe, "vent_suction_active"); return; }

        float phase = std::fmod(s.t, cfg_.period_s) / std::max(0.001f, cfg_.period_s);
        bool burst = phase < cfg_.vent_burst_duty;
        if (!burst) { remove_tag(w, *pe, "vent_suction_active"); return; }

        ensure_tag(w, *pe, "vent_suction_active");
        Vec3 dir{tr.position.x - ptr.position.x, tr.position.y - ptr.position.y, tr.position.z - ptr.position.z};
        float invd = 1.0f / std::max(0.001f, d);
        dir.x *= invd; dir.y *= invd; dir.z *= invd;
        float pull = cfg_.vent_pull_mps * (1.0f - d / std::max(0.001f, cfg_.radius));
        ptr.position.x += dir.x * pull * dt;
        ptr.position.y += dir.y * pull * dt;
        ptr.position.z += dir.z * pull * dt;

        if (d < 0.4f) {
            ensure_tag(w, *pe, "checkpoint_respawn_requested");
        }
    }

    void tick_debris(World& w, std::uint32_t e, Transform& tr, float dt) {
        auto& s = std::get<DebrisState>(state_);
        s.t += dt;
        float cycle = std::fmod(s.t, cfg_.debris_interval_s);
        if (cycle < cfg_.debris_telegraph_s) {
            s.telegraph = true; s.telegraph_t = cycle; ensure_tag(w, e, "debris_telegraph");
        } else {
            if (s.telegraph) {
                s.telegraph = false; remove_tag(w, e, "debris_telegraph");
                resolve_debris_impact(w, e, tr);
            }
        }
    }

    void resolve_debris_impact(World& w, std::uint32_t e, Transform& tr) {
        Vec3 a{tr.position.x + debris_spawn_local_.x, tr.position.y + debris_spawn_local_.y, tr.position.z + debris_spawn_local_.z};
        Vec3 b{tr.position.x + debris_impact_local_.x, tr.position.y + debris_impact_local_.y, tr.position.z + debris_impact_local_.z};
        if (auto pe = find_first_player(w)) {
            if (!w.has<CTransform>(*pe)) return;
            auto pp = w.get<CTransform>(*pe).t.position;
            float dd = segment_point_distance(a, b, pp);
            if (dd <= cfg_.debris_hit_radius) {
                ensure_tag(w, *pe, "debris_hit");
                mission_api::set_score_delta(-5.0f * cfg_.strength);
            }
        }
    }

    void tick_yellow(World& w, std::uint32_t e, Transform& tr, float dt) {
        auto& s = std::get<YellowState>(state_);
        auto pe = find_first_player(w);
        if (!pe || !w.has<CTransform>(*pe)) return;
        auto pp = w.get<CTransform>(*pe).t.position;
        float d = dist(pp, tr.position);
        bool inside = d <= cfg_.radius * tr.scale;
        s.inside = inside;
        approach(s.level, inside ? 1.0f : 0.0f, 3.0f, dt);

        if (s.level > 0.01f) {
            ensure_tag(w, *pe, "yellow_suppression_active");
            ensure_tag(w, *pe, "ring_drain_x1.5");
            ensure_tag(w, *pe, "control_tether_unstable");
        } else {
            remove_tag(w, *pe, "yellow_suppression_active");
            remove_tag(w, *pe, "ring_drain_x1.5");
            remove_tag(w, *pe, "control_tether_unstable");
        }

        if (inside) ensure_tag(w, e, "yellow_zone_active"); else remove_tag(w, e, "yellow_zone_active");
    }

private:
    HazardType type_{HazardType::kUnknown};
    HazardConfig cfg_{};
    StateVariant state_{};

    Vec3 debris_spawn_local_{0.f, 3.f, 0.f};
    Vec3 debris_impact_local_{0.f, 0.f, 0.f};
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.environmental_hazards", ptv::EnvironmentalHazards)
