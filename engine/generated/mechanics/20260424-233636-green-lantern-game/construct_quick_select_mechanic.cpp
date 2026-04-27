#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace ptv {

class ConstructQuickSelect : public IMechanic {
public:
    void on_attach(World&, std::uint32_t, const nlohmann::json& p) override {
        cfg_.lifetime_sec = p.value("lifetime_sec", 30.0f);
        cfg_.cooldown_sec = p.value("cooldown_sec", 0.5f);
        cfg_.platform_altitude_y = p.value("platform_altitude_y", 0.0f);
        cfg_.platform_hover_m = p.value("platform_hover_m", 0.05f);
        cfg_.forward_offset_m = p.value("forward_offset_m", 1.2f);
        cfg_.shield_prefab_id = p.value("shield_prefab_entity_id", std::string{});
        cfg_.hammer_prefab_id = p.value("hammer_prefab_entity_id", std::string{});
        cfg_.platform_prefab_id = p.value("platform_prefab_entity_id", std::string{});
        cfg_.shield_glb_path = p.value("shield_glb_path", std::string{});
        cfg_.hammer_glb_path = p.value("hammer_glb_path", std::string{});
        cfg_.platform_glb_path = p.value("platform_glb_path", std::string{});
        cfg_.shield_scale_m = p.value("shield_scale_m", 1.0f);
        cfg_.hammer_scale_m = p.value("hammer_scale_m", 1.0f);
        cfg_.platform_scale_m = p.value("platform_scale_m", 1.5f);
    }

    void on_update(World& w, std::uint32_t e, float dt) override {
        cleanup_despawns(w, dt);

        // Read dynamic params from binding (harness-driven)
        auto j = read_binding_json(w, e);

        // Allow dynamic overrides
        float lifetime = j.value("lifetime_sec", cfg_.lifetime_sec);
        float cooldown = j.value("cooldown_sec", cfg_.cooldown_sec);
        float platform_alt = j.value("platform_altitude_y", cfg_.platform_altitude_y);
        float hover = j.value("platform_hover_m", cfg_.platform_hover_m);
        float fwd_off = j.value("forward_offset_m", cfg_.forward_offset_m);

        // Dismiss request
        if (j.value("dismiss_all", false)) {
            dismiss_all(w);
        }

        // Input emulation: menu hold + radial selection
        bool held = j.value("menu_held", false);
        int idx_param = j.value("selection_index", -1);
        float angle_deg = j.value("radial_angle_deg", 0.0f);
        std::string construct_str = j.value("construct", std::string{});

        int computed_idx = -1;
        if (idx_param >= 0) {
            computed_idx = clampi(idx_param, 0, 2);
        } else {
            // Map angle to 3-slice (0: shield, 1: hammer, 2: platform)
            float a = std::fmod(angle_deg, 360.0f);
            if (a < 0.0f) a += 360.0f;
            if (a < 120.0f) computed_idx = 0;
            else if (a < 240.0f) computed_idx = 1;
            else computed_idx = 2;
        }
        if (!construct_str.empty()) {
            if (equals_ci(construct_str, "shield")) computed_idx = 0;
            else if (equals_ci(construct_str, "hammer")) computed_idx = 1;
            else if (equals_ci(construct_str, "platform")) computed_idx = 2;
        }

        // Detect release edge
        if (held) {
            current_selection_ = computed_idx;
        }
        if (prev_held_ && !held) {
            // Release -> attempt spawn
            if (cooldown_timer_ <= 0.0f && current_selection_ >= 0) {
                if (w.has<CTransform>(e)) {
                    auto& tr = w.get<CTransform>(e).t;
                    Vec3 aim_dir = read_aim_dir(j);
                    if (is_zero(aim_dir)) {
                        // Fallback to -Z
                        aim_dir = Vec3{0.0f, 0.0f, -1.0f};
                    }
                    normalize_inplace(aim_dir);

                    if (current_selection_ == 0) {
                        spawn_shield(w, tr, aim_dir, fwd_off, platform_alt, lifetime);
                        cooldown_timer_ = cooldown;
                    } else if (current_selection_ == 1) {
                        spawn_hammer(w, tr, aim_dir, fwd_off, lifetime);
                        cooldown_timer_ = cooldown;
                    } else if (current_selection_ == 2) {
                        spawn_platform(w, tr, aim_dir, fwd_off, platform_alt + hover, lifetime);
                        cooldown_timer_ = cooldown;
                    }
                }
            }
            current_selection_ = -1;
        }
        prev_held_ = held;
        cooldown_timer_ = std::max(0.0f, cooldown_timer_ - dt);
    }

    void on_detach(World& w, std::uint32_t) override {
        // Clean up any remaining constructs we spawned
        auto& reg = w.registry();
        for (auto& a : actives_) {
            if (reg.valid(a.e)) {
                reg.destroy(a.e);
            }
        }
        actives_.clear();
    }

    std::string id() const override { return kId(); }

private:
    struct Config {
        float lifetime_sec{30.0f};
        float cooldown_sec{0.5f};
        float platform_altitude_y{0.0f};
        float platform_hover_m{0.05f};
        float forward_offset_m{1.2f};
        std::string shield_prefab_id;
        std::string hammer_prefab_id;
        std::string platform_prefab_id;
        std::string shield_glb_path;
        std::string hammer_glb_path;
        std::string platform_glb_path;
        float shield_scale_m{1.0f};
        float hammer_scale_m{1.0f};
        float platform_scale_m{1.5f};
    } cfg_;

    struct Active {
        entt::entity e{entt::null};
        float ttl{0.0f};
        int type{0}; // 0=shield,1=hammer,2=platform
    };
    std::vector<Active> actives_;
    float cooldown_timer_{0.0f};
    bool prev_held_{false};
    int current_selection_{-1};

    static const char* kId() { return "generated.construct_quick_select_mechanic"; }

    static bool equals_ci(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = (a[i] >= 'A' && a[i] <= 'Z') ? (a[i] - 'A' + 'a') : a[i];
            char cb = (b[i] >= 'A' && b[i] <= 'Z') ? (b[i] - 'A' + 'a') : b[i];
            if (ca != cb) return false;
        }
        return true;
    }
    static int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

    static bool is_zero(const Vec3& v) {
        return std::fabs(v.x) < 1e-5f && std::fabs(v.y) < 1e-5f && std::fabs(v.z) < 1e-5f;
    }
    static void normalize_inplace(Vec3& v) {
        float m2 = v.x*v.x + v.y*v.y + v.z*v.z;
        if (m2 > 1e-8f) {
            float inv = 1.0f / std::sqrt(m2);
            v.x *= inv; v.y *= inv; v.z *= inv;
        }
    }
    static Vec3 mul_add(const Vec3& a, const Vec3& b, float s) {
        return Vec3{a.x + b.x * s, a.y + b.y * s, a.z + b.z * s};
    }

    static Vec3 read_aim_dir(const nlohmann::json& j) {
        if (j.contains("aim_dir") && j.at("aim_dir").is_array() && j.at("aim_dir").size() >= 3) {
            Vec3 d{0,0,0};
            d.x = j.at("aim_dir")[0].get<float>();
            d.y = j.at("aim_dir")[1].get<float>();
            d.z = j.at("aim_dir")[2].get<float>();
            return d;
        }
        float yaw_deg = j.value("aim_yaw_deg", std::numeric_limits<float>::quiet_NaN());
        if (yaw_deg == yaw_deg) {
            float r = yaw_deg * 3.1415926535f / 180.0f;
            // +yaw -> left-handed: x=sin, z=-cos is common in many rigs
            return Vec3{std::sin(r), 0.0f, -std::cos(r)};
        }
        return Vec3{0,0,0};
    }

    nlohmann::json read_binding_json(World& w, std::uint32_t e) {
        if (!w.has<CMechanicBinding>(e)) return nlohmann::json::object();
        auto& mb = w.get<CMechanicBinding>(e);
        // Find the entry for this mechanic
        for (const auto& entry : mb.entries) {
            if (entry.id == kId() || entry.builtin_id == kId()) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object()) return j;
            }
        }
        return nlohmann::json::object();
    }

    void cleanup_despawns(World& w, float dt) {
        auto& reg = w.registry();
        for (auto& a : actives_) {
            a.ttl -= dt;
            if (a.ttl <= 0.0f && reg.valid(a.e)) {
                reg.destroy(a.e);
                a.e = entt::null;
            }
        }
        actives_.erase(std::remove_if(actives_.begin(), actives_.end(),
            [&](const Active& a){ return a.e == entt::null || !reg.valid(a.e); }), actives_.end());
    }

    void dismiss_all(World& w) {
        auto& reg = w.registry();
        for (auto& a : actives_) {
            if (reg.valid(a.e)) reg.destroy(a.e);
            a.e = entt::null;
        }
        actives_.clear();
    }

    // Prefab clone helpers
    static void clone_mesh(World& w, entt::entity from, entt::entity to) {
        auto& reg = w.registry();
        if (reg.all_of<CMesh>(from)) {
            const auto& m = reg.get<CMesh>(from);
            reg.emplace_or_replace<CMesh>(to, CMesh{m.asset_id, m.glb_path, m.target_scale_meters});
        }
    }
    static void clone_physics(World& w, entt::entity from, entt::entity to) {
        auto& reg = w.registry();
        if (reg.all_of<CPhysics>(from)) {
            const auto& p = reg.get<CPhysics>(from);
            reg.emplace_or_replace<CPhysics>(to, CPhysics{p.kind, p.mass_kg, p.restitution, p.friction, p.collision_shape});
        }
    }

    entt::entity find_prefab(World& w, const std::string& id) {
        if (id.empty()) return entt::null;
        entt::entity e = w.find(id);
        return e;
    }

    void add_tags(World& w, entt::entity e, const std::vector<std::string>& extra) {
        CTag t{};
        t.tags = extra;
        w.registry().emplace_or_replace<CTag>(e, std::move(t));
    }

    void ensure_mesh(World& w, entt::entity e, const std::string& glb, float scale) {
        auto& reg = w.registry();
        if (!reg.all_of<CMesh>(e)) {
            CMesh m{};
            m.asset_id = "";
            m.glb_path = glb;
            m.target_scale_meters = scale;
            reg.emplace<CMesh>(e, std::move(m));
        }
    }

    // Spawners
    void spawn_shield(World& w, const Transform& player, const Vec3& dir, float fwd_off, float ground_y, float lifetime) {
        auto& reg = w.registry();
        entt::entity e = reg.create();

        // Transform
        CTransform ct{};
        ct.t.position = mul_add(player.position, dir, fwd_off);
        ct.t.position.y = ground_y;
        ct.t.scale = 1.0f;
        reg.emplace<CTransform>(e, ct);

        // From prefab if available
        entt::entity pf = find_prefab(w, cfg_.shield_prefab_id);
        if (pf != entt::null) {
            clone_mesh(w, pf, e);
            clone_physics(w, pf, e);
        }
        ensure_mesh(w, e, cfg_.shield_glb_path, cfg_.shield_scale_m);

        // Physics: static blocker
        if (!reg.all_of<CPhysics>(e)) {
            CPhysics p{};
            p.kind = "static";
            p.mass_kg = 0.0f;
            p.restitution = 0.05f;
            p.friction = 0.9f;
            p.collision_shape = "box";
            reg.emplace<CPhysics>(e, p);
        }

        add_tags(w, e, {"construct", "shield", "green_emissive", "block_projectiles"});

        actives_.push_back(Active{e, lifetime, 0});
        // Haptics would go here if available.
    }

    void spawn_hammer(World& w, const Transform& player, const Vec3& dir, float fwd_off, float lifetime) {
        auto& reg = w.registry();
        entt::entity e = reg.create();

        // Transform: in front, slightly up
        CTransform ct{};
        ct.t.position = mul_add(player.position, dir, fwd_off);
        ct.t.position.y += 0.15f;
        ct.t.scale = 1.0f;
        reg.emplace<CTransform>(e, ct);

        // Prefab if available
        entt::entity pf = find_prefab(w, cfg_.hammer_prefab_id);
        if (pf != entt::null) {
            clone_mesh(w, pf, e);
            clone_physics(w, pf, e);
        }
        ensure_mesh(w, e, cfg_.hammer_glb_path, cfg_.hammer_scale_m);

        // Dynamic physics
        if (!reg.all_of<CPhysics>(e)) {
            CPhysics p{};
            p.kind = "dynamic";
            p.mass_kg = 5.0f;
            p.restitution = 0.2f;
            p.friction = 0.95f;
            p.collision_shape = "convex";
            reg.emplace<CPhysics>(e, p);
        }

        add_tags(w, e, {"construct", "hammer", "green_emissive", "grabbable", "two_hand_hint"});

        actives_.push_back(Active{e, lifetime, 1});
        // Haptics would go here if available.
    }

    void spawn_platform(World& w, const Transform& player, const Vec3& dir, float fwd_off, float y, float lifetime) {
        auto& reg = w.registry();
        entt::entity e = reg.create();

        // Transform: in front at hover height
        CTransform ct{};
        ct.t.position = mul_add(player.position, dir, fwd_off);
        ct.t.position.y = y;
        ct.t.scale = 1.0f;
        reg.emplace<CTransform>(e, ct);

        // Prefab if available
        entt::entity pf = find_prefab(w, cfg_.platform_prefab_id);
        if (pf != entt::null) {
            clone_mesh(w, pf, e);
            clone_physics(w, pf, e);
        }
        ensure_mesh(w, e, cfg_.platform_glb_path, cfg_.platform_scale_m);

        // Static, high friction
        if (!reg.all_of<CPhysics>(e)) {
            CPhysics p{};
            p.kind = "static";
            p.mass_kg = 0.0f;
            p.restitution = 0.0f;
            p.friction = 1.0f;
            p.collision_shape = "box";
            reg.emplace<CPhysics>(e, p);
        }

        add_tags(w, e, {"construct", "platform", "green_emissive", "standable"});

        actives_.push_back(Active{e, lifetime, 2});
        // Haptics would go here if available.
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.construct_quick_select_mechanic", ptv::ConstructQuickSelect)