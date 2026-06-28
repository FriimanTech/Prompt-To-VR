#include "ptv/api/IMechanic.h"
#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"

#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include <string>
#include <vector>
#include <optional>
#include <cmath>
#include <algorithm>

// Forward declarations for helper functions defined in traversal_ram.cpp
namespace ptv {
struct Vec3; struct Quat; class World; }
namespace traversal_helpers {
ptv::Quat yaw_facing_from_forward(const ptv::Vec3& fwd_ws);
}

// Forward declarations for haptics (provided by dependency exports)
namespace ptv { namespace haptics {
void publish(const std::string& pattern, const std::string& hand_mask, float intensity, std::optional<float> duration_s);
bool has_pattern(const std::string&);
float get_user_intensity();
void set_user_intensity(float);
std::string pattern_to_string(int); // not used, kept for link compatibility if needed
std::string hand_to_string(int);    // not used
} }

namespace ptv {

// Small POD to track lifetimes of spawned constructs (steps, vfx, ram)
struct TimedEntity { entt::entity e{entt::null}; float t_left{0.0f}; };

class TraversalConstructsExtension : public IMechanic {
public:
    std::string id() const override { return "generated.traversal_constructs_extension"; }

    void on_attach(World& world, std::uint32_t entity, const nlohmann::json& params) override {
        (void)world; (void)entity;
        step_spacing_m_ = clampf(params.value("step_spacing_m", 0.35f), 0.15f, 0.8f);
        step_count_     = std::clamp(params.value("step_count", 5), 2, 12);
        ladder_max_len_ = clampf(params.value("ladder_max_length_m", 3.0f), 0.6f, 6.0f);
        ram_speed_mps_  = clampf(params.value("ram_speed_mps", 4.0f), 0.5f, 8.0f);
        ram_stabilize_  = params.value("ram_stabilize", true);
        ram_cancel_on_collision_ = params.value("ram_cancel_on_collision", true);
        attached_entity_ = entity;
        clear_all(world);
    }

    void on_update(World& world, std::uint32_t entity, float dt) override {
        attached_entity_ = entity; // keep updated
        // Refresh live params from our binding entry (harness can update per-frame)
        nlohmann::json p = read_live_params(world, entity);

        // Global cancel from B button (preview or ride)
        bool b_cancel = p.value("b_down", false) || p.value("cancel", false);

        // Update previews and finalize based on mode hints
        const std::string mode = p.value("mode", std::string("auto"));
        if (!ride_active_) {
            // Ladders/steps preview if surface info present or mode explicitly requests it
            if (mode == "ladder" || (p.contains("surface_point_ws") && p.contains("surface_normal_ws"))) {
                update_ladder_preview(world, p);
                if (p.value("confirm", false) || p.value("right_trigger_down", false) || p.value("right_trigger_clicked", false)) {
                    finalize_ladder(world);
                }
            } else {
                destroy_preview(world);
            }
        }

        // Rideable ram flow
        if (!ride_active_ && (mode == "ram" || p.value("construct_ram", false))) {
            if (p.value("confirm", false) || p.value("right_trigger_clicked", false)) {
                spawn_and_mount_ram(world, p);
            }
        }
        if (ride_active_) {
            tick_ram(world, dt, p);
            // Collision/cancel handling
            const bool hit = p.value("hit_something", false) || p.value("ram_collision", false);
            if (b_cancel || (ram_cancel_on_collision_ && hit)) {
                impact_vfx_and_cancel(world);
            }
        }

        // Lifetimes for placed steps and vfx
        tick_lifetimes(world, dt);
    }

    void on_detach(World& world, std::uint32_t) override {
        clear_all(world);
    }

private:
    // Configuration
    float step_spacing_m_{0.35f};
    int   step_count_{5};
    float ladder_max_len_{3.0f};
    float ram_speed_mps_{4.0f};
    bool  ram_stabilize_{true};
    bool  ram_cancel_on_collision_{true};

    // Runtime state
    std::uint32_t attached_entity_{0};
    std::vector<entt::entity> ladder_preview_{};
    std::vector<TimedEntity>  ladder_placed_{}; // 30s lifetime after placement
    std::vector<TimedEntity>  vfx_spawns_{};    // short-lived vfx

    bool ride_active_{false};
    entt::entity ram_{entt::null};
    ptv::Vec3 ram_forward_{0.0f, 0.0f, 1.0f};

private:
    static float clampf(float v, float a, float b) { return std::max(a, std::min(v, b)); }

    nlohmann::json read_live_params(World& world, std::uint32_t e) {
        nlohmann::json out; // empty by default
        if (!world.has<CMechanicBinding>(e)) return out;
        const auto& bind = world.get<CMechanicBinding>(e);
        for (const auto& entry : bind.entries) {
            if (entry.id == id() || entry.builtin_id == id()) {
                auto j = nlohmann::json::parse(entry.params_json, nullptr, false);
                if (j.is_object()) out = j;
                break;
            }
        }
        return out;
    }

    static glm::vec3 to_glm(const ptv::Vec3& v) { return glm::vec3(v.x, v.y, v.z); }
    static ptv::Vec3 to_ptv(const glm::vec3& v) { return ptv::Vec3{v.x, v.y, v.z}; }

    // Ladder preview build/update
    void update_ladder_preview(World& world, const nlohmann::json& p) {
        // Read anchor and normal (defaults if missing)
        ptv::Vec3 anchor{0.0f, 1.0f, 0.0f};
        ptv::Vec3 nrm{0.0f, 1.0f, 0.0f};
        if (p.contains("surface_point_ws") && p["surface_point_ws"].is_array() && p["surface_point_ws"].size() == 3) {
            anchor = ptv::Vec3{p["surface_point_ws"][0].get<float>(), p["surface_point_ws"][1].get<float>(), p["surface_point_ws"][2].get<float>()};
        }
        if (p.contains("surface_normal_ws") && p["surface_normal_ws"].is_array() && p["surface_normal_ws"].size() == 3) {
            nrm = ptv::Vec3{p["surface_normal_ws"][0].get<float>(), p["surface_normal_ws"][1].get<float>(), p["surface_normal_ws"][2].get<float>()};
        }
        // Adjust spacing live with left grip delta if provided
        float spacing = step_spacing_m_;
        if (p.value("left_grip_held", false) && p.contains("spacing_delta")) {
            spacing = clampf(step_spacing_m_ + p.value("spacing_delta", 0.0f), 0.15f, 0.8f);
        }
        int steps = std::clamp(step_count_, 2, 18);
        float total_len = std::min(ladder_max_len_, spacing * (steps - 1));

        // Build basis: along-plane up and right from normal
        glm::vec3 N = glm::normalize(to_glm(nrm));
        glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 tangent = glm::normalize(glm::cross(worldUp, N));
        if (glm::length2(tangent) < 1e-4f) tangent = glm::vec3(1,0,0);
        glm::vec3 up_in_plane = glm::normalize(glm::cross(N, tangent));

        // Ensure we have preview entities
        ensure_preview_entities(world, steps);

        glm::vec3 base = to_glm(anchor);
        // Center the run around anchor, marching away from the surface along up_in_plane
        for (int i = 0; i < steps; ++i) {
            float t = (i / float(steps - 1)) * total_len;
            glm::vec3 pos = base + up_in_plane * t + N * 0.03f; // slightly off the surface to prevent z-fight
            place_preview_step(world, ladder_preview_[i], to_ptv(pos), up_in_plane, N);
        }
    }

    void ensure_preview_entities(World& world, int needed) {
        auto& reg = world.registry();
        // Grow
        while ((int)ladder_preview_.size() < needed) {
            entt::entity e = reg.create();
            reg.emplace<CMesh>(e, CMesh{ "construct_ladder_steps", "assets/construct_ladder_steps.glb", 1.0f });
            reg.emplace<CTransform>(e, CTransform{ Transform{ Vec3{0,0,0}, Quat{1,0,0,0}, 1.0f } });
            reg.emplace<CTag>(e, CTag{ std::vector<std::string>{"construct_preview", "traversal"} });
            ladder_preview_.push_back(e);
        }
        // Shrink
        while ((int)ladder_preview_.size() > needed) {
            auto e = ladder_preview_.back(); ladder_preview_.pop_back();
            if (reg.valid(e)) reg.destroy(e);
        }
    }

    void place_preview_step(World& world, entt::entity e, const ptv::Vec3& pos, const glm::vec3& up_in_plane, const glm::vec3& /*surface_n*/) {
        if (!world.registry().valid(e)) return;
        // Face the crossbar perpendicular to up_in_plane; construct a yaw-only quat for stability
        glm::vec3 fwd = glm::normalize(glm::vec3(up_in_plane.x, 0.0f, up_in_plane.z));
        ptv::Quat q = traversal_helpers::yaw_facing_from_forward(ptv::Vec3{fwd.x, fwd.y, fwd.z});
        auto& tr = world.registry().get<CTransform>(e).t;
        tr.position = pos;
        tr.rotation = q;
        tr.scale = 1.0f;
    }

    void destroy_preview(World& world) {
        auto& reg = world.registry();
        for (auto e : ladder_preview_) if (reg.valid(e)) reg.destroy(e);
        ladder_preview_.clear();
    }

    void finalize_ladder(World& world) {
        if (ladder_preview_.empty()) return;
        auto& reg = world.registry();
        for (auto e : ladder_preview_) {
            if (!reg.valid(e)) continue;
            // Convert to placed: solid physics + timer
            if (!reg.any_of<CPhysics>(e)) {
                reg.emplace<CPhysics>(e, CPhysics{ "static", 1.0f, 0.1f, 0.8f, std::string("mesh") });
            } else {
                auto& ph = reg.get<CPhysics>(e);
                ph.kind = "static"; ph.collision_shape = "mesh"; ph.mass_kg = 1.0f;
            }
            // Update tags
            if (reg.any_of<CTag>(e)) {
                auto& tg = reg.get<CTag>(e).tags;
                if (std::find(tg.begin(), tg.end(), "construct_preview") != tg.end())
                    tg.erase(std::remove(tg.begin(), tg.end(), "construct_preview"), tg.end());
                if (std::find(tg.begin(), tg.end(), "traversal") == tg.end()) tg.push_back("traversal");
            } else {
                reg.emplace<CTag>(e, CTag{ std::vector<std::string>{"traversal"} });
            }
            ladder_placed_.push_back(TimedEntity{ e, 30.0f }); // 30s
        }
        ladder_preview_.clear();
        // Haptics feedback
        try_publish_haptics("form_complete", 1.0f, 0.07f);
    }

    // Rideable Ram -----------------------------------------------------------
    void spawn_and_mount_ram(World& world, const nlohmann::json& p) {
        if (ride_active_) return;
        // Determine spawn transform from hand entity
        if (!world.has<CTransform>(attached_entity_)) return;
        const auto& trh = world.get<CTransform>(attached_entity_).t;
        ptv::Vec3 hand_pos = trh.position;
        ptv::Vec3 fwd = p.contains("aim_direction_ws") && p["aim_direction_ws"].is_array() && p["aim_direction_ws"].size()==3
                        ? ptv::Vec3{p["aim_direction_ws"][0].get<float>(), p["aim_direction_ws"][1].get<float>(), p["aim_direction_ws"][2].get<float>()}
                        : ptv::Vec3{0.0f, 0.0f, 1.0f};
        glm::vec3 F = glm::normalize(glm::vec3(fwd.x, fwd.y, fwd.z));
        if (!std::isfinite(F.x)) F = glm::vec3(0,0,1);
        ptv::Vec3 spawn_pos = ptv::Vec3{ hand_pos.x + F.x * 0.8f, hand_pos.y - 0.2f, hand_pos.z + F.z * 0.8f };
        ptv::Quat yaw_only = traversal_helpers::yaw_facing_from_forward(ptv::Vec3{F.x, 0.0f, F.z});

        auto& reg = world.registry();
        ram_ = reg.create();
        reg.emplace<CMesh>(ram_, CMesh{ "construct_ram_surf", "assets/construct_ram_surf.glb", 1.0f });
        reg.emplace<CTransform>(ram_, CTransform{ Transform{ spawn_pos, yaw_only, 1.0f } });
        reg.emplace<CPhysics>(ram_, CPhysics{ "dynamic", 80.0f, 0.05f, 0.2f, std::string("box") });
        reg.emplace<CTag>(ram_, CTag{ std::vector<std::string>{"rideable", "vehicle_like", "traversal"} });

        ram_forward_ = ptv::Vec3{F.x, 0.0f, F.z};
        ride_active_ = true;
        try_publish_haptics("sharp", 0.9f, 0.05f);
    }

    void tick_ram(World& world, float dt, const nlohmann::json&) {
        if (!ride_active_) return;
        auto& reg = world.registry();
        if (!reg.valid(ram_) || !reg.any_of<CTransform>(ram_)) { ride_active_ = false; ram_ = entt::null; return; }
        auto& tr = reg.get<CTransform>(ram_).t;
        // Move forward (yaw-only forward)
        glm::vec3 F = glm::normalize(glm::vec3(ram_forward_.x, 0.0f, ram_forward_.z));
        float spd = ram_speed_mps_;
        tr.position.x += F.x * spd * dt;
        tr.position.y += F.y * spd * dt; // F.y is 0
        tr.position.z += F.z * spd * dt;
        // Stabilize horizon (no roll)
        if (ram_stabilize_) {
            tr.rotation = traversal_helpers::yaw_facing_from_forward(ptv::Vec3{F.x, 0.0f, F.z});
        }
    }

    void impact_vfx_and_cancel(World& world) {
        auto& reg = world.registry();
        // Spawn a brief impact VFX at ram nose if available
        if (reg.valid(ram_) && reg.any_of<CTransform>(ram_)) {
            const auto& tr = reg.get<CTransform>(ram_).t;
            entt::entity v = reg.create();
            reg.emplace<CMesh>(v, CMesh{ "construct_energy_vfx_core", "assets/construct_energy_vfx_core.glb", 1.0f });
            reg.emplace<CTransform>(v, CTransform{ Transform{ tr.position, tr.rotation, 1.0f } });
            reg.emplace<CTag>(v, CTag{ std::vector<std::string>{"vfx"} });
            vfx_spawns_.push_back(TimedEntity{ v, 0.6f });
        }
        // Destroy ram
        if (reg.valid(ram_)) reg.destroy(ram_);
        ram_ = entt::null; ride_active_ = false;
        try_publish_haptics("impact_strong", 1.0f, 0.08f);
    }

    // Lifetimes --------------------------------------------------------------
    void tick_lifetimes(World& world, float dt) {
        auto& reg = world.registry();
        auto tick_container = [&](std::vector<TimedEntity>& v){
            for (auto& te : v) te.t_left -= dt;
            for (auto& te : v) if (te.t_left <= 0.0f) { if (reg.valid(te.e)) reg.destroy(te.e); }
            v.erase(std::remove_if(v.begin(), v.end(), [&](const TimedEntity& te){ return te.t_left <= 0.0f; }), v.end());
        };
        tick_container(ladder_placed_);
        tick_container(vfx_spawns_);
    }

    void clear_all(World& world) {
        destroy_preview(world);
        auto& reg = world.registry();
        for (auto& te : ladder_placed_) if (reg.valid(te.e)) reg.destroy(te.e);
        ladder_placed_.clear();
        for (auto& te : vfx_spawns_) if (reg.valid(te.e)) reg.destroy(te.e);
        vfx_spawns_.clear();
        if (reg.valid(ram_)) reg.destroy(ram_);
        ram_ = entt::null; ride_active_ = false;
    }

    void try_publish_haptics(const std::string& pattern, float intensity, float dur_s) {
        try {
            ptv::haptics::publish(pattern, std::string("right"), intensity, dur_s);
        } catch (...) { /* no-op if service not present */ }
    }
};

} // namespace ptv

PTV_REGISTER_MECHANIC("generated.traversal_constructs_extension", ptv::TraversalConstructsExtension)
