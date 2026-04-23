#include "ptv/core/Application.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include <GLFW/glfw3.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ptv/core/Logger.h"
#include "ptv/debug/DebugUI.h"
#include "ptv/scene/SceneLoader.h"

namespace ptv {

// ---------------------------------------------------------------------------
// XR view / FOV helpers
// ---------------------------------------------------------------------------

static Mat4 xr_pose_to_matrix(const XrPosef& p) {
    Quat q(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
    Vec3 t(p.position.x, p.position.y, p.position.z);
    return glm::translate(Mat4(1.0f), t) * glm::mat4_cast(q);
}

static Quat xr_pose_to_quat(const XrPosef& p) {
    return Quat(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
}

static Mat4 rig_world_from_stage(float pos_x, float pos_y, float pos_z,
                                  float yaw_rad) {
    Mat4 m = glm::translate(Mat4(1.0f), Vec3(pos_x, pos_y, pos_z));
    m = glm::rotate(m, yaw_rad, Vec3(0.0f, 1.0f, 0.0f));
    return m;
}

static Mat4 xr_fov_to_projection(const XrFovf& fov, float near_z, float far_z) {
    const float tan_left = std::tan(fov.angleLeft);
    const float tan_right = std::tan(fov.angleRight);
    const float tan_down = std::tan(fov.angleDown);
    const float tan_up = std::tan(fov.angleUp);

    const float tan_width = tan_right - tan_left;
    const float tan_height = tan_up - tan_down;

    Mat4 m(0.0f);
    m[0][0] = 2.0f / tan_width;
    m[1][1] = 2.0f / tan_height;
    m[2][0] = (tan_right + tan_left) / tan_width;
    m[2][1] = (tan_up + tan_down) / tan_height;
    m[2][2] = -far_z / (far_z - near_z);
    m[2][3] = -1.0f;
    m[3][2] = -(far_z * near_z) / (far_z - near_z);
    return m;
}

static std::string resolve_scene_path(const Application::Config& cfg) {
    if (!cfg.scene_json_path.empty()) return cfg.scene_json_path;
    if (const char* env = std::getenv("PTV_SCENE_JSON")) {
        if (env && *env) return env;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Procedural hand visual: palm + wrist stub + thumb extrusion.
//
// The mesh is designed in the OpenXR "grip" local frame:
//   +Z = toward the user's thumb (same for both hands, per the spec)
//   -Z = toward the user's wrist / forearm
// The palm is drawn symmetric in X so no per-hand mirroring is needed,
// at the cost of not visually distinguishing back-of-hand vs palm-side.
// This reads correctly in VR and matches the level of detail people
// expect from "controller glove" style visuals.
// ---------------------------------------------------------------------------
static void push_hand_mesh(std::vector<Renderer::DrawItem>& out,
                           const Mat4& grip_world,
                           const Vec4& color) {
    auto push = [&](const Vec3& local_translate, const Vec3& local_scale) {
        Renderer::DrawItem item;
        Mat4 local = glm::translate(Mat4(1.0f), local_translate) *
                     glm::scale(Mat4(1.0f), local_scale);
        item.model = grip_world * local;
        item.color = color;
        item.material_id = 2.0f;   // hand material (rim highlight)
        out.push_back(item);
    };

    // Palm: oblong rounded-cuboid. Width across (X), height front/back (Y),
    // length along the thumb->pinky axis (Z).
    push(Vec3(0.0f, 0.0f, 0.0f),
         Vec3(0.055f, 0.035f, 0.11f));

    // Thumb: small box extending along +Z, slightly above/forward of palm.
    push(Vec3(0.0f, 0.012f, 0.075f),
         Vec3(0.028f, 0.028f, 0.05f));

    // Wrist / forearm stub: extends along -Z so the hand looks attached
    // to something instead of floating as a free palm.
    push(Vec3(0.0f, 0.0f, -0.085f),
         Vec3(0.045f, 0.045f, 0.07f));
}

// Extract the XY position in world from a transform matrix (column 3).
static Vec3 mat_translation(const Mat4& m) {
    return Vec3(m[3][0], m[3][1], m[3][2]);
}

// ---------------------------------------------------------------------------
// Grab helpers
// ---------------------------------------------------------------------------

// Find the nearest entity tagged "grabbable" within ``max_dist`` of ``pos``.
// Returns ``entt::null`` if nothing is in range.
static entt::entity find_grabbable(World& world, const Vec3& pos,
                                    float max_dist) {
    entt::entity best = entt::null;
    float best_d2 = max_dist * max_dist;
    auto view = world.registry().view<const CTransform, const CTag>();
    for (auto e : view) {
        const auto& tags = view.get<const CTag>(e).tags;
        bool grabbable = false;
        for (const auto& t : tags) {
            if (t == "grabbable") { grabbable = true; break; }
        }
        if (!grabbable) continue;
        const auto& ct = view.get<const CTransform>(e).t;
        Vec3 dp = ct.position - pos;
        float d2 = glm::dot(dp, dp);
        if (d2 < best_d2) {
            best_d2 = d2;
            best = e;
        }
    }
    return best;
}

int Application::run(const Config& cfg) {
    if (!initialize(cfg)) {
        shutdown();
        return 2;
    }

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    running_ = true;

    std::vector<Renderer::DrawItem> items;
    items.reserve(128);

    while (running_) {
        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        if (dt > 0.1f) dt = 0.1f;  // clamp long stalls
        last = now;
        world_.set_delta_time(dt);

        window_->poll_events();
        if (window_->should_close()) running_ = false;

        if (!xr_.poll_events()) running_ = false;

        XrRuntime::InputState input;
        xr_.sync_input(input);

        // ------------------------------------------------------------
        // Thumbstick locomotion + snap turn (unchanged).
        // ------------------------------------------------------------
        {
            constexpr float kMoveDeadzone = 0.15f;
            constexpr float kMoveSpeed = 2.0f;
            constexpr float kTurnDeadzone = 0.7f;
            constexpr float kSnapTurnDeg = 45.0f;
            constexpr float kSnapCooldown = 0.30f;

            auto apply_deadzone = [](float v, float dz) -> float {
                float s = (v < 0.0f) ? -1.0f : 1.0f;
                float m = std::fabs(v);
                if (m < dz) return 0.0f;
                return s * (m - dz) / (1.0f - dz);
            };

            const float lx = apply_deadzone(input.thumbstick[0].x, kMoveDeadzone);
            const float ly = apply_deadzone(input.thumbstick[0].y, kMoveDeadzone);
            if (lx != 0.0f || ly != 0.0f) {
                const float cy = std::cos(rig_.yaw_rad);
                const float sy = std::sin(rig_.yaw_rad);
                const float fwd_x = -sy;
                const float fwd_z = -cy;
                const float right_x = cy;
                const float right_z = -sy;
                rig_.pos_x += (ly * fwd_x + lx * right_x) * kMoveSpeed * dt;
                rig_.pos_z += (ly * fwd_z + lx * right_z) * kMoveSpeed * dt;
            }

            rig_.snap_cooldown_s = std::max(0.0f, rig_.snap_cooldown_s - dt);
            const float rx = input.thumbstick[1].x;
            if (rig_.snap_cooldown_s <= 0.0f) {
                if (rx > kTurnDeadzone) {
                    rig_.yaw_rad -= kSnapTurnDeg * 3.14159265f / 180.0f;
                    rig_.snap_cooldown_s = kSnapCooldown;
                } else if (rx < -kTurnDeadzone) {
                    rig_.yaw_rad += kSnapTurnDeg * 3.14159265f / 180.0f;
                    rig_.snap_cooldown_s = kSnapCooldown;
                }
            }
        }

        // The rig transforms the stage origin into the world. We need
        // this to compute world-space hand poses for grab detection and
        // for the per-frame kinematic follow.
        const Mat4 rig_mat = rig_world_from_stage(
            rig_.pos_x, rig_.pos_y, rig_.pos_z, rig_.yaw_rad);

        // Compose world-space hand matrices once (reused for grab +
        // rendering to keep visual & physics in perfect lockstep).
        const bool hand_valid[2] = {input.hand_left_valid, input.hand_right_valid};
        const XrPosef hand_poses[2] = {input.hand_left.pose, input.hand_right.pose};
        Mat4 hand_world[2]{Mat4(1.0f), Mat4(1.0f)};
        Vec3 hand_world_pos[2]{Vec3(0.0f), Vec3(0.0f)};
        Quat hand_world_rot[2]{Quat(1,0,0,0), Quat(1,0,0,0)};
        for (int h = 0; h < 2; ++h) {
            if (!hand_valid[h]) continue;
            hand_world[h] = rig_mat * xr_pose_to_matrix(hand_poses[h]);
            hand_world_pos[h] = mat_translation(hand_world[h]);
            // Rig rotation is yaw only, so we can compose rotations directly.
            Quat rig_yaw = glm::angleAxis(rig_.yaw_rad, Vec3(0, 1, 0));
            hand_world_rot[h] = rig_yaw * xr_pose_to_quat(hand_poses[h]);
        }

        // ------------------------------------------------------------
        // Grab: grip (squeeze) edge-triggered. Press-to-grab nearest
        // grabbable within 15cm; release-to-drop + throw impulse based
        // on the smoothed per-frame hand velocity.
        // ------------------------------------------------------------
        constexpr float kGrabPress = 0.7f;
        constexpr float kGrabRelease = 0.4f;
        constexpr float kGrabRadius = 0.18f;
        constexpr float kThrowScale = 1.5f;   // multiply measured velocity

        for (int h = 0; h < 2; ++h) {
            auto& hs = hands_[h];

            // Smooth hand velocity for a stable throw impulse on release.
            if (hand_valid[h]) {
                if (hs.had_prev_pos && dt > 0.0f) {
                    Vec3 inst = (hand_world_pos[h] - hs.prev_world_pos) / dt;
                    // EMA: alpha=0.4 is a good balance between responsiveness
                    // and filtering out the jitter of the Quest 3 cameras.
                    hs.world_velocity = hs.world_velocity * 0.6f + inst * 0.4f;
                }
                hs.prev_world_pos = hand_world_pos[h];
                hs.had_prev_pos = true;
            } else {
                hs.had_prev_pos = false;
                hs.world_velocity = Vec3(0.0f);
            }

            const float grip_now = input.grip[h];
            const bool was_pressed = hs.prev_grip >= kGrabPress;
            const bool is_pressed  = grip_now >= kGrabPress;
            const bool is_released = grip_now < kGrabRelease;

            if (!was_pressed && is_pressed && hs.held == entt::null && hand_valid[h]) {
                // Edge press -> try to grab nearest grabbable.
                entt::entity target = find_grabbable(world_, hand_world_pos[h],
                                                     kGrabRadius);
                if (target != entt::null) {
                    hs.held = target;
                    // Compute hold offset in hand-local frame so the cube
                    // keeps its relative orientation to the hand when grabbed.
                    const auto& ct = world_.registry().get<CTransform>(target).t;
                    Mat4 inv_hand = glm::inverse(hand_world[h]);
                    Vec4 local_p = inv_hand * Vec4(ct.position, 1.0f);
                    hs.hold_local_offset = Vec3(local_p);
                    Quat inv_hand_q = glm::inverse(hand_world_rot[h]);
                    hs.hold_local_rot = inv_hand_q * ct.rotation;
                    // Release the OTHER hand if it's holding the same entity.
                    auto& other = hands_[1 - h];
                    if (other.held == target) {
                        other.held = entt::null;
                    }
                    physics_.set_kinematic(
                        static_cast<std::uint32_t>(target), true);
                    PTV_INFO("Grab: hand {} -> entity {}",
                             h == 0 ? "L" : "R",
                             static_cast<uint32_t>(target));
                }
            } else if (was_pressed && is_released && hs.held != entt::null) {
                // Edge release -> drop + throw.
                auto held = hs.held;
                physics_.set_kinematic(
                    static_cast<std::uint32_t>(held), false);
                physics_.apply_impulse(
                    static_cast<std::uint32_t>(held),
                    hs.world_velocity * kThrowScale *
                        /* scaled by mass ~0.4kg in scene */ 0.4f);
                PTV_INFO("Release: hand {} drops entity {} @ {:.2f} m/s",
                         h == 0 ? "L" : "R",
                         static_cast<uint32_t>(held),
                         glm::length(hs.world_velocity));
                hs.held = entt::null;
            }

            hs.prev_grip = grip_now;
        }

        // ------------------------------------------------------------
        // Drive kinematic held bodies BEFORE the physics step so Jolt
        // integrates their motion and resolves contacts correctly.
        // ------------------------------------------------------------
        for (int h = 0; h < 2; ++h) {
            auto& hs = hands_[h];
            if (hs.held == entt::null || !hand_valid[h]) continue;
            // Compose the target world transform from the recorded
            // offset so the cube feels attached to the hand.
            Vec3 target_pos = hand_world_pos[h] +
                              hand_world_rot[h] * hs.hold_local_offset;
            Quat target_rot = hand_world_rot[h] * hs.hold_local_rot;
            physics_.move_kinematic(static_cast<std::uint32_t>(hs.held),
                                    target_pos, target_rot, dt);
        }

        // ------------------------------------------------------------
        // Physics + mechanics (unchanged order, but now happens AFTER
        // locomotion + grab so kinematic targets are applied this step).
        // ------------------------------------------------------------
        physics_.step(dt);
        physics_.sync_to_world(world_);
        mechanics_.update(world_, dt);

        // ------------------------------------------------------------
        // Render frame.
        // ------------------------------------------------------------
        if (renderer_ready_ && xr_.is_running()) {
            renderer_.build_items_from_world(world_, items);

            // Hand visuals: procedural palm + wrist + thumb in blue (left)
            // and red (right). The grab highlights light up brighter when
            // a grabbable is in range so the user knows where to squeeze.
            for (int h = 0; h < 2; ++h) {
                if (!hand_valid[h]) continue;
                const bool hovering = hands_[h].held != entt::null ||
                    find_grabbable(world_, hand_world_pos[h],
                                   kGrabRadius) != entt::null;
                const Vec4 left_base{0.35f, 0.55f, 1.00f, 1.0f};
                const Vec4 right_base{1.00f, 0.45f, 0.40f, 1.0f};
                Vec4 c = (h == 0) ? left_base : right_base;
                if (hovering) c = c * 1.35f + Vec4(0.08f, 0.08f, 0.08f, 0.0f);
                c.a = 1.0f;
                push_hand_mesh(items, hand_world[h], c);
            }

            xr_.render_frame(
                [&](const std::array<XrRuntime::EyeImage, 2>& eyes,
                    XrTime /*t*/) {
                    std::array<Renderer::EyeTarget, 2> targets{};
                    for (int e = 0; e < 2; ++e) {
                        targets[e].image = eyes[e].image;
                        targets[e].offset_x = 0;
                        targets[e].offset_y = 0;
                        targets[e].width = eyes[e].width;
                        targets[e].height = eyes[e].height;
                        Mat4 world_from_head =
                            rig_mat * xr_pose_to_matrix(eyes[e].pose);
                        targets[e].view = glm::inverse(world_from_head);
                        targets[e].projection = xr_fov_to_projection(
                            eyes[e].fov, 0.05f, 1000.0f);
                    }
                    renderer_.draw_frame(targets, items, scene_.sky_color);
                });
        }

        DebugUI::new_frame();
        XrFrameInfo fi{dt * 1000.0f, window_->width(), window_->height()};
        DebugUI::draw(world_, fi);
        DebugUI::render_to_mirror_window();

        tick(dt);
    }

    shutdown();
    return 0;
}

bool Application::initialize(const Config& cfg) {
    try {
        window_ = std::make_unique<Window>(cfg.mirror_width, cfg.mirror_height,
                                            cfg.app_name + " [mirror]");
    } catch (const std::exception& e) {
        PTV_ERROR("Window init: {}", e.what());
        return false;
    }

    PTV_INFO("App: beginning OpenXR initialization (phase 1: instance/system)...");
    bool xr_ready = xr_.init_instance_and_system(cfg.app_name);

    VulkanContext::Config vcfg;
    vcfg.app_name = cfg.app_name;
    vcfg.enable_validation = cfg.enable_validation;
    vcfg.required_instance_extensions =
        XrRuntime::required_vulkan_instance_extensions();

    if (xr_ready) {
        PTV_INFO("App: Vulkan initialize_for_openxr...");
        if (!vk_.initialize_for_openxr(vcfg,
                                        xr_.instance(),
                                        xr_.system(),
                                        xr_.runtime_min_vulkan_api(),
                                        xr_.runtime_max_vulkan_api())) {
            PTV_WARN("Vulkan(OpenXR) init failed; will try non-XR fallback.");
            xr_ready = false;
        }
    }
    if (!xr_ready) {
        PTV_INFO("App: Vulkan plain initialize (no XR)...");
        if (!vk_.initialize(vcfg)) return false;
    }

    if (xr_ready) {
        PTV_INFO("App: OpenXR phase 2 (session + swapchains)...");
        if (!xr_.init_session_and_swapchains(vk_)) {
            PTV_WARN("OpenXR session init failed. Mirror window will still run.");
        }
    } else {
        PTV_WARN("OpenXR init failed earlier. Is Meta Quest Link running and"
                 " set as the active OpenXR runtime?");
    }
    PTV_INFO("App: OpenXR initialization returned");

    if (!physics_.initialize()) return false;

    scene_path_ = resolve_scene_path(cfg);
    if (!scene_path_.empty()) {
        scene_ = LoadSceneJson(scene_path_);
        ApplySceneToWorld(scene_, world_);
        physics_.rebuild_from_world(world_);
    } else {
        PTV_WARN("No scene.json provided. The engine will run idle.");
    }

    mechanics_.attach_bindings(world_);
    // NOTE: DebugUI is intentionally NOT initialized here. See
    // README / troubleshooting: ImGui >= 1.92 asserts on first Begin()
    // without a renderer backend that sets RendererHasTextures.

    if (xr_.is_running() || xr_.xr_session() != XR_NULL_HANDLE) {
        renderer_ready_ = renderer_.initialize(vk_, xr_.color_format(),
                                               xr_.eye_width(),
                                               xr_.eye_height());
        if (!renderer_ready_) {
            PTV_WARN("Renderer init failed; headset will not display content.");
        }
    }
    return true;
}

void Application::tick(float /*dt*/) {}

void Application::shutdown() {
    mechanics_.detach_all(world_);
    DebugUI::shutdown();
    renderer_.shutdown();
    renderer_ready_ = false;
    xr_.shutdown();
    physics_.shutdown();
    vk_.shutdown();
    window_.reset();
}

}  // namespace ptv
