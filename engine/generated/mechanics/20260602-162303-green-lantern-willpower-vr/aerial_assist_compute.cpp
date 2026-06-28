#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <optional>
#include <algorithm>

namespace ptv { namespace hud_helpers {
    void hud_get_settings_json(std::string& out_json);
} }

namespace {

inline float clamp01(float v){ return std::max(0.0f, std::min(1.0f, v)); }

inline float rad2deg(float r){ return r * 57.2957795f; }
inline float deg2rad(float d){ return d * 0.01745329252f; }

std::optional<float> smallest_positive_root(float a, float b, float c) {
    if (std::fabs(a) < 1e-6f) {
        if (std::fabs(b) < 1e-6f) return std::nullopt;
        float t = -c / b;
        return t > 0.0f ? std::optional<float>(t) : std::nullopt;
    }
    float disc = b*b - 4*a*c;
    if (disc < 0.0f) return std::nullopt;
    float s = std::sqrt(disc);
    float t1 = (-b - s) / (2*a);
    float t2 = (-b + s) / (2*a);
    float t = 1e30f;
    if (t1 > 0.0f) t = std::min(t, t1);
    if (t2 > 0.0f) t = std::min(t, t2);
    if (t == 1e30f) return std::nullopt;
    return t;
}

ptv::Vec3 lerp(const ptv::Vec3& a, const ptv::Vec3& b, float t){ return ptv::Vec3{ a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t }; }

float dot(const ptv::Vec3& a, const ptv::Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }

float length(const ptv::Vec3& v){ return std::sqrt(dot(v,v)); }

ptv::Vec3 normalize(const ptv::Vec3& v){ float L = length(v); if (L < 1e-6f) return ptv::Vec3{0,0,1}; return ptv::Vec3{ v.x/L, v.y/L, v.z/L }; }

float smoothing_alpha(float dt, float tau){ if (tau <= 0.0f) return 1.0f; return 1.0f - std::exp(-dt / tau); }

struct InputSample {
    bool left_trigger_held{false};
    float right_stick_y{0.0f};

    bool have_target{false};
    ptv::Vec3 target_pos{0,0,0};
    ptv::Vec3 target_vel{0,0,0};
    ptv::Vec3 aim_origin{0,0,0};
    ptv::Vec3 aim_dir{0,0,1};
    float projectile_speed_mps{20.0f};
};

InputSample read_input_from_binding(ptv::World& w, std::uint32_t e, float fallback_projectile_speed) {
    InputSample s; s.projectile_speed_mps = fallback_projectile_speed;
    if (!w.has<ptv::CMechanicBinding>(e)) return s;
    const auto& bind = w.get<ptv::CMechanicBinding>(e);
    for (const auto& entry : bind.entries) {
        if (entry.id == std::string("generated.aerial_combat_assist")) {
            auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
            if (!j.is_object()) continue;
            s.left_trigger_held = j.value("left_trigger_held", false);
            s.right_stick_y     = j.value("right_stick_y", 0.0f);
            s.projectile_speed_mps = j.value("projectile_speed_mps", fallback_projectile_speed);
            if (j.contains("target") && j["target"].is_object()) {
                auto jt = j["target"];
                if (jt.contains("pos") && jt.contains("vel")) {
                    auto p = jt["pos"]; auto v = jt["vel"];
                    if (p.is_array() && p.size()==3 && v.is_array() && v.size()==3) {
                        s.target_pos = ptv::Vec3{ p[0].get<float>(), p[1].get<float>(), p[2].get<float>() };
                        s.target_vel = ptv::Vec3{ v[0].get<float>(), v[1].get<float>(), v[2].get<float>() };
                        s.have_target = true;
                    }
                }
            }
            if (j.contains("aim") && j["aim"].is_object()) {
                auto ja = j["aim"];
                if (ja.contains("origin") && ja.contains("dir")) {
                    auto o = ja["origin"]; auto d = ja["dir"];
                    if (o.is_array() && o.size()==3 && d.is_array() && d.size()==3) {
                        s.aim_origin = ptv::Vec3{ o[0].get<float>(), o[1].get<float>(), o[2].get<float>() };
                        s.aim_dir    = normalize(ptv::Vec3{ d[0].get<float>(), d[1].get<float>(), d[2].get<float>() });
                    }
                }
            }
            break;
        }
    }
    return s;
}

std::optional<ptv::Vec3> predict_intercept(const ptv::Vec3& shooter_pos,
                                           const ptv::Vec3& target_pos,
                                           const ptv::Vec3& target_vel,
                                           float projectile_speed)
{
    ptv::Vec3 r{ target_pos.x - shooter_pos.x, target_pos.y - shooter_pos.y, target_pos.z - shooter_pos.z };
    ptv::Vec3 v = target_vel;
    float vv = dot(v,v);
    float ss = projectile_speed * projectile_speed;
    auto t = smallest_positive_root(vv - ss, 2.0f * dot(r,v), dot(r,r));
    if (!t.has_value()) return std::nullopt;
    ptv::Vec3 lead{ target_pos.x + v.x * (*t), target_pos.y + v.y * (*t), target_pos.z + v.z * (*t) };
    return lead;
}

std::string vec3_to_str(const ptv::Vec3& v){
    return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
}

void put_ctag_kv(ptv::World& w, std::uint32_t e, const std::string& key, const std::string& value){
    ptv::CTag tag;
    if (w.has<ptv::CTag>(e)) tag = w.get<ptv::CTag>(e);
    const std::string needle = key + "=";
    bool replaced = false;
    for (auto& t : tag.tags){
        if (t.rfind(needle, 0) == 0){ t = needle + value; replaced = true; break; }
    }
    if (!replaced) tag.tags.push_back(needle + value);
    w.emplace_or_replace<ptv::CTag>(e, tag);
}

bool query_ui_enabled_or(std::optional<bool> fallback){
    std::string blob; ptv::hud_helpers::hud_get_settings_json(blob);
    if (!blob.empty()){
        auto j = nlohmann::json::parse(blob, nullptr, false);
        if (j.is_object()){
            if (j.contains("combat") && j["combat"].is_object()){
                auto c = j["combat"];
                if (c.contains("aerial_assist_enabled")) return c.value("aerial_assist_enabled", fallback.value_or(true));
                if (c.contains("target_assist"))        return c.value("target_assist",        fallback.value_or(true));
            }
            if (j.contains("aerial_assist_enabled")) return j.value("aerial_assist_enabled", fallback.value_or(true));
            if (j.contains("target_assist"))        return j.value("target_assist",        fallback.value_or(true));
        }
    }
    return fallback.value_or(true);
}

} // anon

namespace ptv {

class AerialCombatAssist : public IMechanic {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override;
    void on_update(World& world, std::uint32_t entity, float dt) override;
    void on_detach(World& world, std::uint32_t entity) override;
    std::string id() const override { return "generated.aerial_combat_assist"; }

private:
    bool  enabled_{true};
    float lead_stiffness_{0.6f};
    float magnetism_cone_deg_{5.0f};
    float magnetism_strength_{0.15f};
    float altitude_nudge_step_m_{0.2f};
    float projectile_speed_mps_{20.0f};
    Vec3 smoothed_lead_{0.0f, 0.0f, 0.0f};
    bool have_smoothed_{false};
};

void AerialCombatAssist::on_attach(World&, std::uint32_t, const nlohmann::json& params) {
    enabled_               = params.value("enable", true);
    lead_stiffness_        = params.value("lead_stiffness", 0.6f);
    magnetism_cone_deg_    = params.value("magnetism_cone_deg", 5.0f);
    magnetism_strength_    = params.value("magnetism_strength", 0.15f);
    altitude_nudge_step_m_ = params.value("altitude_nudge_step_m", 0.2f);
    projectile_speed_mps_  = params.value("projectile_speed_mps", 20.0f);
    smoothed_lead_ = ptv::Vec3{0,0,0};
    have_smoothed_ = false;
}

void AerialCombatAssist::on_update(World& w, std::uint32_t e, float dt) {
    if (!w.valid(e)) return;

    enabled_ = query_ui_enabled_or(enabled_);

    InputSample in = read_input_from_binding(w, e, projectile_speed_mps_);

    if (enabled_ && std::fabs(in.right_stick_y) > 0.5f && w.has<CTransform>(e)) {
        auto& tr = w.get<CTransform>(e).t;
        float step = altitude_nudge_step_m_ * (in.right_stick_y > 0.0f ? 1.0f : -1.0f);
        tr.position.y += step;
    }

    put_ctag_kv(w, e, "aimassist.enabled", enabled_ ? "1" : "0");

    if (!enabled_ || !in.have_target) {
        if (have_smoothed_) {
            float a = clamp01(smoothing_alpha(dt, std::max(0.05f, lead_stiffness_)) * 0.5f);
            smoothed_lead_ = lerp(smoothed_lead_, ptv::Vec3{ in.aim_origin.x + in.aim_dir.x*5.0f,
                                                             in.aim_origin.y + in.aim_dir.y*5.0f,
                                                             in.aim_origin.z + in.aim_dir.z*5.0f }, a);
            put_ctag_kv(w, e, "aimassist.lead_world", vec3_to_str(smoothed_lead_));
        }
        return;
    }

    auto lead_opt = predict_intercept(in.aim_origin, in.target_pos, in.target_vel, in.projectile_speed_mps);
    ptv::Vec3 lead = lead_opt.value_or(in.target_pos);

    float alpha = clamp01(smoothing_alpha(dt, std::max(0.02f, lead_stiffness_)));
    if (!have_smoothed_) { smoothed_lead_ = lead; have_smoothed_ = true; }
    else                 { smoothed_lead_ = lerp(smoothed_lead_, lead, alpha); }

    put_ctag_kv(w, e, "aimassist.lead_world", vec3_to_str(smoothed_lead_));

    ptv::Vec3 to_lead = ptv::Vec3{ smoothed_lead_.x - in.aim_origin.x,
                                   smoothed_lead_.y - in.aim_origin.y,
                                   smoothed_lead_.z - in.aim_origin.z };
    ptv::Vec3 to_lead_dir = normalize(to_lead);
    float cosang = clamp01((dot(in.aim_dir, to_lead_dir) + 1.0f) * 0.5f) * 2.0f - 1.0f;
    float ang_deg = rad2deg(std::acos(clamp01((dot(in.aim_dir, to_lead_dir) + 1.0f)*0.5f)*2.0f - 1.0f));

    float cone = std::max(1.0f, magnetism_cone_deg_);
    float t = clamp01(1.0f - (ang_deg / cone));
    float blend = magnetism_strength_ * t;

    ptv::Vec3 magnet_dir = normalize(ptv::Vec3{ in.aim_dir.x * (1.0f - blend) + to_lead_dir.x * blend,
                                                in.aim_dir.y * (1.0f - blend) + to_lead_dir.y * blend,
                                                in.aim_dir.z * (1.0f - blend) + to_lead_dir.z * blend });

    if (in.left_trigger_held) {
        float extra = std::min(0.25f, magnetism_strength_ * 0.5f);
        float locked_blend = std::min(0.6f, blend + extra);
        magnet_dir = normalize(ptv::Vec3{ in.aim_dir.x * (1.0f - locked_blend) + to_lead_dir.x * locked_blend,
                                          in.aim_dir.y * (1.0f - locked_blend) + to_lead_dir.y * locked_blend,
                                          in.aim_dir.z * (1.0f - locked_blend) + to_lead_dir.z * locked_blend });
        put_ctag_kv(w, e, "aimassist.lock_refine", "1");
    } else {
        put_ctag_kv(w, e, "aimassist.lock_refine", "0");
    }

    put_ctag_kv(w, e, "aimassist.magnet_dir", vec3_to_str(magnet_dir));
    put_ctag_kv(w, e, "aimassist.aim_dir",     vec3_to_str(in.aim_dir));
    (void)cosang;
}

void AerialCombatAssist::on_detach(World& w, std::uint32_t e) {
    if (w.has<ptv::CTag>(e)) {
        auto tag = w.get<ptv::CTag>(e);
        auto& vec = tag.tags;
        auto keep = std::vector<std::string>{}; keep.reserve(vec.size());
        for (const auto& t : vec) {
            if (t.rfind("aimassist.", 0) != 0) keep.push_back(t);
        }
        tag.tags = std::move(keep);
        w.emplace_or_replace<ptv::CTag>(e, tag);
    }
}

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.aerial_combat_assist", ptv::AerialCombatAssist)
