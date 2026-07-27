#pragma once

#include <array>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <vector>

#include "ptv/ecs/World.h"
#include "ptv/math/Math.h"
#include "ptv/mechanics/MechanicRegistry.h"
#include "ptv/physics/PhysicsWorld.h"
#if !defined(__ANDROID__)
// GLFW mirror window is desktop-only. On Quest (NativeActivity) there is no
// desktop surface, and Window's implementation (platform/desktop/Window.cpp)
// is excluded from the Android build -- so even holding a unique_ptr<Window>
// member would odr-use the missing Window::~Window() at link time.
#include "ptv/platform/Window.h"
#endif
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
        // Primary scene path (from explicit argv[1] or a launcher). Takes
        // priority over PTV_SCENE_JSON and the fallback below.
        std::string scene_json_path;
        // Sidecar/demo scene (e.g. the starter-room's baked-in scene.json
        // next to the exe). Only used when neither argv[1] nor the
        // PTV_SCENE_JSON env var is provided -- so launching a generated
        // project no longer accidentally loads the sample room.
        std::string scene_json_fallback_path;
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
    // Build the renderer's environment (sky/sun/ambient/fog/point lights) from
    // the loaded ``scene_``. Cached so it isn't rebuilt every frame; defaults
    // are chosen so scenes without an authored sky/lights (e.g. the starter
    // room) still look good.
    void build_render_env();

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

    // ------------------------------------------------------------------
    // Locomotion: the Starter Room prompts the user once on load for
    // their preferred movement style. The same enum / router is reused
    // in every generated game so new prompts don't need to rewire input.
    //
    // Terminology follows Meta's Horizon "Locomotion Types" guide:
    //   - "Teleport" is a discrete locomotion (aim + commit).
    //   - "Sliding" is Meta's name for thumbstick continuous motion.
    //   - "Head-Relative" sliding moves along the user's gaze.
    //   - "Controller-Relative" sliding moves along the left controller's
    //     pointing direction, independent of head yaw.
    // Refs:
    //   https://developers.meta.com/horizon/design/locomotion-types/
    //   https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types
    //   https://developers.meta.com/horizon/design/locomotion-input-maps/
    // ------------------------------------------------------------------
    enum class LocomotionMode : int {
        Teleport = 0,                // left-stick push: aim arc; release: teleport
        SlideHeadRelative = 1,       // left-stick: slide in the direction of head gaze
        SlideControllerRelative = 2, // left-stick: slide along the left controller's aim
    };
    enum class MenuPage : int {
        Root = 0,           // "Pick your Movement Style"
        SlidingSubmenu = 1, // "Pick your Sliding Style"
        Closed = 2,         // user confirmed; menu removed
    };
    struct Menu {
        MenuPage page{MenuPage::Root};
        int highlighted{-1};        // -1 = nothing, 0/1 = per-page options
        // Which controller is currently aiming at the highlighted pad
        // (-1 = neither). Both controllers can drive the menu; whichever
        // hand is pointing closer to a pad wins, and that hand's trigger
        // edge confirms the selection.
        int active_hand{-1};
        float confirm_cooldown_s{0.5f};    // debounce so the menu doesn't
                                           // instantly eat the first click
        float prev_trigger[2]{0.0f, 0.0f}; // [0]=left, [1]=right
        // The menu panel is placed once (when first shown) in front of the
        // user's eyes (HMD position). Default is a fallback used only
        // until the first valid HMD pose has been observed.
        Vec3 world_anchor{0.0f, 1.55f, -1.2f}; // default in-stage pose
        float yaw_rad{0.0f};
        bool anchor_set{false};
    };

    // Teleport aim + commit state. The arc is drawn from the left
    // controller with gravity, landing on the first y=0 intersection.
    struct TeleportAim {
        bool aiming{false};
        bool have_target{false};
        Vec3 target{0.0f};
        std::vector<Vec3> arc_points;   // debug render segments (in world)
    };

#if !defined(__ANDROID__)
    std::unique_ptr<Window> window_;  // desktop mirror window only
#endif
    VulkanContext vk_{};
    XrRuntime xr_{};
    Renderer renderer_{};
    bool renderer_ready_{false};
    World world_{};
    PhysicsWorld physics_{};
    MechanicHost mechanics_{};
    SceneDoc scene_{};
    std::string scene_path_{};
    Renderer::RenderEnv render_env_{};
    bool running_{false};
    PlayerRig rig_{};
    std::array<HandGrab, 2> hands_{};
    // Previous frame's raw OpenXR input, kept so the per-frame InputSnapshot
    // handed to mechanics can expose edge transitions (trigger/grip/button
    // rising + falling edges).
    XrRuntime::InputState prev_input_{};
    LocomotionMode locomotion_{LocomotionMode::Teleport};
    Menu menu_{};
    TeleportAim teleport_{};
    // Horizontal head-facing direction in **stage / world XZ** (Y=0 plane).
    //
    // CRITICAL: this MUST default to the zero vector so that until we have
    // observed a real HMD pose, *no* slide direction is available and the
    // player simply does not move. A non-zero default like (0, 0, -1)
    // causes "world-based movement": pushing the joystick in head-relative
    // mode would slide the player in fixed world -Z regardless of where
    // they're physically looking. The slide path explicitly skips motion
    // when this length is zero (`glm::length(last_head_forward_xz_) <= 1e-4f`).
    //
    // Updated every frame from ``InputState::head`` *before* locomotion
    // math runs, so head-relative slide always uses the latest HMD yaw.
    Vec3 last_head_forward_xz_{0.0f, 0.0f, 0.0f};
    // Latest cached HMD eye world position (full XYZ). Updated inside the
       // input/locomotion block each frame. Used to anchor the locomotion menu
    // at a comfortable eye-relative height (Meta Horizon "Head -> Comfort"
    // guidance: place primary UI ~1.5m forward, slightly below eye level).
    Vec3 last_head_world_pos_{0.0f, 0.0f, 0.0f};
    bool last_head_pose_valid_{false};

    // Pre-loaded GLB icons for the locomotion menu (root: Walking + Teleport,
    // submenu: Head-Relative + Controller-Relative). -1 = not loaded yet
    // (renderer falls back to procedural icons in that case).
    int menu_icon_walking_slot_{-1};
    int menu_icon_teleport_slot_{-1};
    int menu_icon_head_relative_slot_{-1};
    int menu_icon_controller_relative_slot_{-1};

    // Comfort acceleration ramp for sliding locomotion. Meta's design
    // guide recommends easing into and out of motion to reduce vection
    // sickness; we lerp `current_slide_speed_mps_` toward the requested
    // target each frame instead of snapping.
    float current_slide_speed_mps_{0.0f};

    // Locomotion tunables (loaded from scene.json ``player`` block; see
    // docs/locomotion.md). Defaults match Meta's comfort recommendations.
    bool locomotion_menu_enabled_{true};
    float slide_max_speed_mps_{1.5f};
    float slide_accel_tau_s_{0.12f};
    float slide_decel_tau_s_{0.18f};
    float teleport_launch_speed_mps_{6.0f};
    float teleport_max_horizontal_m_{12.0f};
    float teleport_floor_y_{0.0f};
    float snap_turn_degrees_{45.0f};
    float snap_turn_cooldown_s_{0.30f};
};

}  // namespace ptv
