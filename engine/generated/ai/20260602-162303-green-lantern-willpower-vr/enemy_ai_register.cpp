#include "ptv/api/IBehaviour.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>

#include <string>
#include <vector>
#include <array>
#include <optional>
#include <algorithm>
#include <cmath>

namespace enemy_ai {
// Forward declarations implemented in separate .cpp files
void ground_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                 const ptv::Vec3& player_vel, float dt, int tier, float& local_t);
void drone_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                const ptv::Vec3& player_vel, float dt, int tier, float& local_t);
void turret_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                 const ptv::Vec3& player_vel, float dt, int tier, float& local_t);
void brute_tick(ptv::World& w, std::uint32_t self, const ptv::Vec3& player_pos,
                const ptv::Vec3& player_vel, float dt, int tier, float& local_t);
}

namespace {
inline float dot(const ptv::Vec3& a, const ptv::Vec3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float len(const ptv::Vec3& v){ return std::sqrt(dot(v,v)); }
inline ptv::Vec3 norm(const ptv::Vec3& v){ float l=len(v); return (l>1e-5f)? ptv::Vec3{v.x/l,v.y/l,v.z/l} : ptv::Vec3{0,0,0}; }
}

namespace ptv {

class EnemyAICombatPack final : public IBehaviour {
public:
    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        tier_ = std::max(1, params.value("mission_tier", 1));
        role_ = detect_role(world, entity);
        local_t_ = 0.0f;
    }

    void on_tick(World& world, std::uint32_t entity, float dt) override {
        if (!world.valid(entity)) return;
        local_t_ += dt;

        // Acquire player reference (nearest entity tagged player_avatar)
        auto player_info = find_player(world);
        if (!player_info.has_value()) return;
        const auto player_e = player_info->first;
        const auto player_tr = player_info->second;
        const Vec3 player_pos = player_tr.position;
        const Vec3 player_vel = estimate_player_velocity(world, player_e);

        switch (role_) {
            case Role::Ground:
                enemy_ai::ground_tick(world, entity, player_pos, player_vel, dt, tier_, local_t_);
                break;
            case Role::Drone:
                enemy_ai::drone_tick(world, entity, player_pos, player_vel, dt, tier_, local_t_);
                break;
            case Role::Turret:
                enemy_ai::turret_tick(world, entity, player_pos, player_vel, dt, tier_, local_t_);
                break;
            case Role::Brute:
                enemy_ai::brute_tick(world, entity, player_pos, player_vel, dt, tier_, local_t_);
                break;
        }

        // Always face the player gently (readability); do not force camera
        if (world.has<CTransform>(entity)) {
            auto& t = world.get<CTransform>(entity).t;
            Vec3 fwd = norm(Vec3{player_pos.x - t.position.x, 0.0f, player_pos.z - t.position.z});
            (void)fwd; // Rotation not modified here due to Quat ops being engine-defined elsewhere.
        }
    }

    void on_detach(World&, std::uint32_t) override {}

    std::string id() const override { return "generated.enemy_ai_combat_pack"; }

private:
    enum class Role { Ground, Drone, Turret, Brute };

    static Role detect_role(World& w, std::uint32_t e) {
        // Prefer tags for classification; fallback to mesh asset_id
        if (w.has<CTag>(e)) {
            const auto& tags = w.get<CTag>(e).tags;
            if (contains_tag(tags, "turret")) return Role::Turret;
            if (contains_tag(tags, "aerial")) return Role::Drone;
            if (contains_tag(tags, "elite") || contains_tag(tags, "suppression_zone")) return Role::Brute;
        }
        if (w.has<CMesh>(e)) {
            const auto& a = w.get<CMesh>(e).asset_id;
            if (a.find("sniper_turret") != std::string::npos) return Role::Turret;
            if (a.find("aerial_drone") != std::string::npos) return Role::Drone;
            if (a.find("fear_corrupted_brute") != std::string::npos) return Role::Brute;
        }
        return Role::Ground;
    }

    static bool contains_tag(const std::vector<std::string>& tags, const std::string& t) {
        return std::find(tags.begin(), tags.end(), t) != tags.end();
    }

    static std::optional<std::pair<std::uint32_t, Transform>> find_player(World& w) {
        std::optional<std::pair<std::uint32_t, Transform>> best;
        float best_d2 = std::numeric_limits<float>::infinity();
        auto& reg = w.registry();
        auto view = reg.view<CTransform, CTag>();
        for (auto ent : view) {
            auto eid = static_cast<std::uint32_t>(ent);
            const auto& tag = view.get<CTag>(ent);
            if (std::find(tag.tags.begin(), tag.tags.end(), std::string("player_avatar")) == tag.tags.end())
                continue;
            const auto& tr = view.get<CTransform>(ent).t;
            // Prefer closest to origin of enemies if multiple; here just pick nearest to world origin for determinism
            float d2 = dot(tr.position, tr.position);
            if (d2 < best_d2) { best_d2 = d2; best = std::make_pair(eid, tr); }
        }
        return best;
    }

    static Vec3 estimate_player_velocity(World& w, std::uint32_t player_e) {
        // No explicit velocity component provided; simple finite diff cache via a tag is overkill.
        // Return zero; sub-behaviours can still perform lead via dt-local motion.
        (void)w; (void)player_e; return Vec3{0,0,0};
    }

private:
    Role role_{Role::Ground};
    int  tier_{1};
    float local_t_{0.0f};
};

PTV_REGISTER_BEHAVIOUR("generated.enemy_ai_combat_pack", EnemyAICombatPack)

} // namespace ptv
