#pragma once

#include <array>
#include <entt/entt.hpp>
#include <memory>
#include <string>

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include "ptv/mechanics/MechanicRegistry.h"
#include "ptv/physics/PhysicsWorld.h"
#include "ptv/platform/Window.h"
#include "ptv/render/Renderer.h"
#include "ptv/render/VulkanContext.h"
#include "ptv/scene/Scene.h"
#include "ptv/xr/XrSession.h"

namespace ptv {

/** Top-level application class.
 *
 * Owns the Vulkan context, the OpenXR session, the ECS world, the physics
 * world, and the mechanic host. The ``run()`` method is the main loop.
 */
class Application {
public:
    struct Config {
        std::string app_name{"ptv_runtime"};
        std::string scene_json_path;        // path to scene.json (from PTV_SCENE_JSON or argv[1])
        int mirror_width{1280};
        int mirror_height{720};
        bool enable_validation{true};
    };

    Application() = default;
    ~Application() = default;

    int run(const Config& cfg);

private:
    bool initialize(const Config& cfg);
    void shutdown();
    void tick(float dt);

    // ------------------------------------------------------------------
    // Player rig: the virtual transform of the stage origin in world
    // space. Thumbstick-driven locomotion moves/rotates the rig; the
    // physical head/hand poses from OpenXR are then composed through it.
    // ------------------------------------------------------------------
    struct PlayerRig {
        float pos_x{0.0f};
        float pos_y{0.0f};
        float pos_z{0.0f};
        float yaw_rad{0.0f};          // rotation around world Y
        float snap_cooldown_s{0.0f};  // prevents rapid repeated snap turns
    };

    // Per-hand grab state tracked across frames so we can detect edge
    // transitions (press-to-grab / release-to-throw) and compute a
    // velocity estimate for the throw impulse.
    struct HandGrab {
        entt::entity held{entt::null};
        float prev_grip{0.0f};       // previous frame's grip (squeeze) value
        Vec3 prev_world_pos{0.0f};   // previous frame hand world position
        Vec3 world_velocity{0.0f};   // smoothed m/s
        bool had_prev_pos{false};
        Vec3 hold_local_offset{0.0f};        // entity pos in hand-local frame
        Quat hold_local_rot{1, 0, 0, 0};     // entity rot in hand-local frame
    };

    std::unique_ptr<Window> window_;
    VulkanContext vk_{};
    XrRuntime xr_{};
    Renderer renderer_{};
    bool renderer_ready_{false};
    World world_{};
    PhysicsWorld physics_{};
    MechanicHost mechanics_{};
    SceneDoc scene_{};
    std::string scene_path_{};
    bool running_{false};
    PlayerRig rig_{};
    std::array<HandGrab, 2> hands_{};
};

}  // namespace ptv
