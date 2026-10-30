#pragma once

// The engine side of ctx.projectiles (ptv/api/Projectiles.h).
//
// A fixed pool of projectiles, none of which is an ECS entity or a physics
// body. Each frame every live projectile integrates gravity and drag and
// tests the path it just travelled with a swept ray / sphere that ignores its
// shooter, the weapon and whatever the shooter holds -- continuous collision
// by construction. Mesh visuals (arrows, rockets) are drawn through a small
// pool of render-only proxy entities: a CMesh + CTransform copied from the
// projectile's template, hidden with the "template" tag when idle, so the
// renderer's LOD, culling and streaming apply to them like anything else.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ptv/api/MechanicContext.h"
#include "ptv/ecs/World.h"

namespace ptv {

class ProjectileSystem final : public IProjectiles {
public:
    struct Services {
        World* world{nullptr};
        IPhysicsAccess* physics{nullptr};
        ICombat* combat{nullptr};
        IEffects* vfx{nullptr};
        IHands* hands{nullptr};
    };

    void configure(int max_live, int max_mesh_visuals);
    void bind(const Services& s) { svc_ = s; }
    /** Scene change: forget every projectile and proxy entity. */
    void clear();

    std::uint32_t launch(const ProjectileDesc& desc) override;
    int live_count() const override;
    std::uint64_t hit_seq() const override { return hit_seq_; }
    int hits_since(std::uint64_t after_seq, ProjectileHit* out, int max_out) const override;

    void tick(float dt);

    /** The scene's designed projectiles; ``template_name`` resolves to the
     *  hidden mesh template when ``preset`` is asked. */
    struct Preset {
        std::string id;
        std::string template_name;
        ProjectileDesc desc{};
    };
    void set_presets(std::vector<Preset> presets) { presets_ = std::move(presets); }
    bool preset(const char* projectile_id, ProjectileDesc* out) const override;

    /** Ray / sphere casts made in the last tick (telemetry). */
    int casts_last_tick() const { return casts_; }

private:
    enum class State { Free, Flying, Stuck, Resting };
    struct Live {
        State state{State::Free};
        std::uint32_t id{0};
        ProjectileDesc desc{};
        Vec3 pos{0.0f};
        Vec3 vel{0.0f};
        float age{0.0f};
        float travelled{0.0f};
        float state_age{0.0f};
        // Stuck: the entity it rides, and its pose in that entity's frame.
        std::uint32_t stuck_to{0xFFFFFFFFu};
        Mat4 stuck_local{1.0f};
        Quat rot{1.0f, 0.0f, 0.0f, 0.0f};
        int visual{-1};  // index into visuals_
        int bounces{0};
    };
    struct Visual {
        std::uint32_t entity{0xFFFFFFFFu};
        std::uint32_t template_entity{0xFFFFFFFFu};
        bool in_use{false};
        Vec3 nose_local{0.0f};
        Vec3 nose_dir{0.0f, 0.0f, 1.0f};
    };

    void step(Live& p, float dt);
    void hit(Live& p, const PhysicsRayHit& h, const Vec3& dir);
    void expire(Live& p);
    void explode(Live& p, const Vec3& at, std::uint32_t direct_target);
    std::uint32_t health_owner(std::uint32_t entity) const;
    bool may_damage(std::uint32_t shooter, std::uint32_t target) const;
    void record_hit(const Live& p, std::uint32_t target, const Vec3& point,
                    const Vec3& normal, float damage, bool killed);
    int acquire_visual(std::uint32_t template_entity);
    void release_visual(Live& p);
    void place_visual(const Live& p, const Quat& rot);
    int build_ignore(const Live& p, std::uint32_t* out, int cap) const;

    Services svc_{};
    std::vector<Preset> presets_;
    std::vector<Live> pool_;
    std::vector<Visual> visuals_;
    int max_mesh_visuals_{24};
    std::uint32_t next_id_{1};
    std::array<ProjectileHit, 64> hits_{};
    std::uint64_t hit_seq_{0};
    int casts_{0};
};

}  // namespace ptv
