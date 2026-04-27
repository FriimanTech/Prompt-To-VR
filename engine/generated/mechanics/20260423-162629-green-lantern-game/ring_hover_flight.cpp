#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ptv {

static inline float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }
static inline float length2(const Vec3& v) { return v.x*v.x + v.y*v.y + v.z*v.z; }
static inline float length(const Vec3& v) { return std::sqrt(length2(v)); }
static inline Vec3 normalize_safe(const Vec3& v) { float l = length(v); return l > 1e-5f ? Vec3{v.x/l, v.y/l, v.z/l} : Vec3{0,0,0}; }
static inline float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

class RingHoverFlight : public IMechanic {
public:
    void on_attach(World&, std::uint32_t e, const nlohmann::json& p) override {
        // Settings
        s_.speed_max = p.value("speed_mps_max", 6.0f);
        s_.accel = p.value("accel_mps2", 10.0f);
        s_.ascent = p.value("ascent_rate_mps", 2.0f);
        s_.alt_cap = p.value("altitude_cap_m", 8.0f);
        s_.ground_y = p.value("ground_y", 0.0f);

        auto ejson = p.contains("energy") ? p.at("energy") : nlohmann::json::object();
        s_.drain_per_sec = ejson.value("drain_per_sec", 2.0f);
        s_.min_to_engage = ejson.value("min_to_engage", 5.0f);
        float initial_energy = ejson.value("initial", ejson.value("start", 20.0f));

        auto cjson = p.contains("comfort") ? p.at("comfort") : nlohmann::json::object();
        s_.vig_min = cjson.value("vignette_min", 0.2f);
        s_.vig_max = cjson.value("vignette_max", 0.7f);
        s_.vig_speed_max = std::max(0.1f, cjson.value("speed_for_max", s_.speed_max));

        auto vjson = p.contains("volumes") ? p.at("volumes") : nlohmann::json::object();
        s_.allowed_tag = vjson.value("allowed_tag", std::string("hover_allowed"));
        s_.no_fly_tag = vjson.value("no_fly_tag", std::string("no_fly"));
        s_.soft_bound_tag = vjson.value("soft_bound_tag", std::string("soft_bound"));
        s_.soft_margin_frac = clampf(vjson.value("soft_margin_fraction", 0.15f), 0.0f, 0.9f);

        s_.scene_id = p.value("scene_id", std::string());
        s_.indoor_disallow = p.value("indoor", false);
        s_.snap_turn_active = p.value("snap_turn_active", false);

        s_.unlocked = p.value("flight_unlocked", false);
        if (!s_.unlocked && p.contains("unlock_flag_key")) {
            std::string key = p.at("unlock_flag_key").get<std::string>();
            if (p.contains("flags") && p.at("flags").contains(key)) {
                s_.unlocked = p.at("flags").at(key).get<bool>();
            }
        }
        if (p.contains("scenes_enabled")) {
            try {
                for (auto& x : p.at("scenes_enabled")) s_.scenes_enabled.push_back(x.get<std::string>());
            } catch (...) {}
        }

        auto& st = states_[e];
        st.energy = initial_energy;
        st.active = false;
        st.v = Vec3{0,0,0};
        st.vignette = 0.0f;
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        if (dt <= 0.0f) dt = std::max(0.0f, w.delta_time());
        auto& reg = w.registry();
        if (!w.has<CTransform>(e)) return;
        auto& tr = w.get<CTransform>(e).t;
        auto& st = states_[e];

        // Input (from params via binding). Defaults are safe and offline-friendly.
        // Grip to engage, planar move (x=right, y=forward), vertical: up/down or scalar.
        bool grip = read_param_bool(w, e, "grip_held", false) || read_param_bool(w, e, "hover_held", false);
        float mx = read_param_float(w, e, "move_x", 0.0f);
        float my = read_param_float(w, e, "move_y", 0.0f);
        float vert = 0.0f;
        if (has_param(w, e, "vertical")) vert = read_param_float(w, e, "vertical", 0.0f);
        else {
            bool up = read_param_bool(w, e, "ascend", false) || read_param_bool(w, e, "lift_up", false);
            bool dn = read_param_bool(w, e, "descend", false) || read_param_bool(w, e, "lift_down", false);
            vert = (up ? 1.0f : 0.0f) - (dn ? 1.0f : 0.0f);
        }

        // Environment gates
        bool in_enabled_scene = s_.scenes_enabled.empty() || (!s_.scene_id.empty() && in_list(s_.scene_id, s_.scenes_enabled));
        bool env_ok = s_.unlocked && in_enabled_scene && !s_.indoor_disallow;

        // Volumes: derive simple spherical zones from tagged entities (center at CTransform.position, radius from CMesh.target_scale_meters or default)
        const auto allowed = gather_spheres_with_tag(reg, s_.allowed_tag);
        const auto nofly = gather_spheres_with_tag(reg, s_.no_fly_tag);
        const auto softb = gather_spheres_with_tag(reg, s_.soft_bound_tag);

        Vec3 pos = tr.position;
        if (!allowed.empty()) {
            if (!point_in_any(pos, allowed)) env_ok = false;
        }
        if (point_in_any(pos, nofly)) env_ok = false;

        // Decide active hover
        bool can_engage_energy = (st.energy >= s_.min_to_engage);
        bool want_active = grip && env_ok && can_engage_energy;
        if (!want_active) st.active = false;
        else st.active = true;

        // Energy drain
        if (st.active) {
            st.energy -= s_.drain_per_sec * dt;
            if (st.energy <= 0.0f) {
                st.energy = 0.0f;
                st.active = false;
            }
        }

        // Motion update
        if (st.active) {
            // Desired acceleration from input
            Vec3 input_dir = normalize_safe(Vec3{mx, 0.0f, my});
            Vec3 a{input_dir.x * s_.accel, 0.0f, input_dir.z * s_.accel};

            // Vertical control
            float target_vy = clampf(vert, -1.0f, 1.0f) * s_.ascent;
            float dv_y = target_vy - st.v.y;
            float ay = clampf(dv_y / std::max(1e-3f, dt), -s_.accel, s_.accel);
            a.y = ay;

            // Integrate velocity
            st.v.x += a.x * dt;
            st.v.z += a.z * dt;
            st.v.y += a.y * dt;

            // Horizontal speed cap
            Vec3 vh{st.v.x, 0.0f, st.v.z};
            float spd = length(vh);
            if (spd > s_.speed_max) {
                vh = normalize_safe(vh);
                vh.x *= s_.speed_max; vh.z *= s_.speed_max;
                st.v.x = vh.x; st.v.z = vh.z;
            }

            // Gentle vertical stabilization when no vertical input
            if (std::abs(vert) < 1e-3f) {
                st.v.y *= std::exp(-4.0f * dt); // damping towards hover
            }

            // Altitude cap
            float cap_y = s_.ground_y + s_.alt_cap;
            if (tr.position.y >= cap_y && st.v.y > 0.0f) st.v.y = 0.0f;

            // Soft boundary pushback
            apply_soft_bounds(softb, s_.soft_margin_frac, tr.position, st.v, dt);

            // Apply position update
            Vec3 newpos{tr.position.x + st.v.x * dt,
                        tr.position.y + st.v.y * dt,
                        tr.position.z + st.v.z * dt};

            // Enforce hard soft-bound outer radius (no exit)
            clamp_to_soft_bounds(softb, newpos);

            // Enforce altitude hard cap
            if (newpos.y > cap_y) {
                newpos.y = cap_y;
                if (st.v.y > 0.0f) st.v.y = 0.0f;
            }

            tr.position = newpos;

            // Idle drag if no input
            if (length2(input_dir) < 1e-4f) {
                float drag = std::exp(-3.0f * dt);
                st.v.x *= drag; st.v.z *= drag;
            }
        } else {
            // Not active: bleed off mechanic velocity
            st.v.x *= std::exp(-6.0f * dt);
            st.v.y *= std::exp(-6.0f * dt);
            st.v.z *= std::exp(-6.0f * dt);
        }

        // Comfort vignette intensity (store locally; a real integration would drive a post-effect)
        float horiz_speed = std::sqrt(st.v.x*st.v.x + st.v.z*st.v.z);
        float t = clampf(horiz_speed / std::max(0.1f, s_.vig_speed_max), 0.0f, 1.0f);
        st.vignette = s_.vig_min + (s_.vig_max - s_.vig_min) * t;

        // HUD/UI persistence are not available; a real implementation would publish st.active, st.energy, st.vignette via UI/save APIs.
    }

    void on_detach(World&, std::uint32_t e) override {
        states_.erase(e);
    }

    std::string id() const override { return "generated.ring_hover_flight"; }

private:
    struct Settings {
        float speed_max{6.0f};
        float accel{10.0f};
        float ascent{2.0f};
        float alt_cap{8.0f};
        float ground_y{0.0f};

        float drain_per_sec{2.0f};
        float min_to_engage{5.0f};

        float vig_min{0.2f};
        float vig_max{0.7f};
        float vig_speed_max{6.0f};

        std::string allowed_tag{"hover_allowed"};
        std::string no_fly_tag{"no_fly"};
        std::string soft_bound_tag{"soft_bound"};
        float soft_margin_frac{0.15f};

        std::vector<std::string> scenes_enabled;
        std::string scene_id;
        bool indoor_disallow{false};
        bool snap_turn_active{false};
        bool unlocked{false};
    } s_;

    struct State {
        bool active{false};
        float energy{0.0f};
        Vec3 v{0.0f,0.0f,0.0f};
        float vignette{0.0f};
    };
    std::unordered_map<std::uint32_t, State> states_;

    // CMechanicBinding::Entry::params_json is a serialized STRING (stable across
    // save/load), so we have to parse it before reading keys.
    static nlohmann::json parse_entry_params(const std::string& s) {
        if (s.empty()) return nlohmann::json::object();
        return nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
    }
    static bool has_param(World& w, std::uint32_t e, const char* key) {
        if (!w.has<CMechanicBinding>(e)) return false;
        const auto& entries = w.get<CMechanicBinding>(e).entries;
        for (const auto& en : entries) {
            auto j = parse_entry_params(en.params_json);
            if (j.is_object() && j.contains(key)) return true;
        }
        return false;
    }
    static bool read_param_bool(World& w, std::uint32_t e, const char* key, bool defv) {
        if (!w.has<CMechanicBinding>(e)) return defv;
        const auto& entries = w.get<CMechanicBinding>(e).entries;
        for (const auto& en : entries) {
            auto j = parse_entry_params(en.params_json);
            if (j.is_object() && j.contains(key)) {
                try { return j.at(key).get<bool>(); } catch (...) {}
            }
        }
        return defv;
    }
    static float read_param_float(World& w, std::uint32_t e, const char* key, float defv) {
        if (!w.has<CMechanicBinding>(e)) return defv;
        const auto& entries = w.get<CMechanicBinding>(e).entries;
        for (const auto& en : entries) {
            auto j = parse_entry_params(en.params_json);
            if (j.is_object() && j.contains(key)) {
                try { return j.at(key).get<float>(); } catch (...) {}
            }
        }
        return defv;
    }

    static bool tag_has(const CTag& t, const std::string& tag) {
        for (const auto& x : t.tags) if (x == tag) return true;
        return false;
    }
    static bool in_list(const std::string& s, const std::vector<std::string>& L) {
        for (auto& x : L) if (x == s) return true;
        return false;
    }

    struct Sphere { Vec3 c; float r; };

    static float mesh_radius_or_default(entt::registry& reg, entt::entity ent, float defr) {
        if (reg.all_of<CMesh>(ent)) {
            const auto& m = reg.get<CMesh>(ent);
            if (m.target_scale_meters > 0.0f) return m.target_scale_meters;
        }
        return defr;
    }

    static std::vector<Sphere> gather_spheres_with_tag(entt::registry& reg, const std::string& tag) {
        std::vector<Sphere> out;
        auto view = reg.view<CTransform, CTag>();
        for (auto ent : view) {
            const auto& tags = view.get<CTag>(ent);
            if (!tag_has(tags, tag)) continue;
            const auto& t = view.get<CTransform>(ent).t;
            float r = mesh_radius_or_default(reg, ent, 5.0f);
            out.push_back(Sphere{t.position, std::max(0.1f, r)});
        }
        return out;
    }

    static bool point_in_any(const Vec3& p, const std::vector<Sphere>& sv) {
        for (const auto& s : sv) {
            Vec3 d{p.x - s.c.x, p.y - s.c.y, p.z - s.c.z};
            if (length2(d) <= s.r * s.r) return true;
        }
        return false;
    }

    static void apply_soft_bounds(const std::vector<Sphere>& sv, float margin_frac, const Vec3& pos, Vec3& vel, float dt) {
        if (sv.empty()) return;
        // Use the nearest soft sphere
        const Sphere* best = nullptr;
        float bestd2 = std::numeric_limits<float>::max();
        for (const auto& s : sv) {
            Vec3 d{pos.x - s.c.x, pos.y - s.c.y, pos.z - s.c.z};
            float d2 = length2(d);
            if (d2 < bestd2) { bestd2 = d2; best = &s; }
        }
        if (!best) return;
        Vec3 d{pos.x - best->c.x, pos.y - best->c.y, pos.z - best->c.z};
        float r = best->r;
        float dist = std::sqrt(std::max(0.0f, length2(d)));
        if (dist < 1e-4f) return;
        Vec3 n = Vec3{d.x / dist, d.y / dist, d.z / dist};
        float outer = r;
        float inner = r * (1.0f - clampf(margin_frac, 0.0f, 0.9f));
        if (dist >= inner) {
            float t = clampf((dist - inner) / std::max(1e-3f, outer - inner), 0.0f, 1.0f);
            float outward_speed = dot(vel, n);
            // Dampen outward velocity and add inward corrective acceleration
            if (outward_speed > 0.0f) {
                vel.x -= outward_speed * n.x * (0.5f + 0.5f * t);
                vel.y -= outward_speed * n.y * (0.5f + 0.5f * t);
                vel.z -= outward_speed * n.z * (0.5f + 0.5f * t);
            }
            float pull = (1.0f + 4.0f * t); // stronger near edge
            vel.x -= n.x * pull * dt;
            vel.y -= n.y * pull * dt;
            vel.z -= n.z * pull * dt;
        }
    }

    static void clamp_to_soft_bounds(const std::vector<Sphere>& sv, Vec3& pos) {
        if (sv.empty()) return;
        // Clamp to nearest sphere if outside
        const Sphere* best = nullptr;
        float bestd2 = std::numeric_limits<float>::max();
        for (const auto& s : sv) {
            Vec3 d{pos.x - s.c.x, pos.y - s.c.y, pos.z - s.c.z};
            float d2 = length2(d);
            if (d2 < bestd2) { bestd2 = d2; best = &s; }
        }
        if (!best) return;
        Vec3 d{pos.x - best->c.x, pos.y - best->c.y, pos.z - best->c.z};
        float r = best->r;
        float dist = length(d);
        if (dist > r && dist > 1e-4f) {
            Vec3 n = Vec3{d.x / dist, d.y / dist, d.z / dist};
            pos.x = best->c.x + n.x * r * 0.999f;
            pos.y = best->c.y + n.y * r * 0.999f;
            pos.z = best->c.z + n.z * r * 0.999f;
        }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.ring_hover_flight", ptv::RingHoverFlight)