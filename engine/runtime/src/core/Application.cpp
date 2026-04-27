#include "ptv/core/Application.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Locomotion + startup menu (lives entirely in this file).
//
// Terminology matches Meta's "Locomotion Types" design guide:
//   * Teleport -- discrete: aim arc + commit.
//   * Sliding (Meta's term for thumbstick continuous motion). We expose
//     two flavours per Meta's input-map guidance:
//       - SlideHeadRelative: stick steers along the HMD gaze direction.
//       - SlideControllerRelative: stick steers along the left controller's
//         pointing direction (head-independent).
// References:
//   https://developers.meta.com/horizon/design/locomotion-types/
//   https://developers.meta.com/horizon/design/locomotion-types/#artificial-locomotion-types
//   https://developers.meta.com/horizon/design/locomotion-input-maps/
//
// Flow:
//  * On session start ``menu_.page == Root``. A 3D panel with two option
//    pads ("TELEPORT" / "SLIDING") is drawn directly via Renderer::DrawItem
//    at a world-space anchor placed in front of the user's spawn pose.
//  * Each frame we cast a ray from the left controller along its forward
//    direction and find which option pad the ray is closest to; the hit
//    pad is highlighted, and a pulsing green confirm quad appears next
//    to it. Pulling the left trigger (edge transition) commits the
//    selection.
//  * Choosing TELEPORT closes the menu with ``LocomotionMode::Teleport``.
//    Choosing SLIDING flips the menu to ``SlidingSubmenu`` which shows
//    two new pads: HEAD-RELATIVE (yellow HMD-shape) and CONTROLLER-RELATIVE
//    (purple controller-shape). A second confirm closes the menu with the
//    matching ``LocomotionMode``.
//  * Until the menu closes, normal locomotion is disabled so the user
//    can't accidentally fly away while deciding.
//  * After the menu closes:
//      - Teleport:               Left stick magnitude > threshold => aim a cyan
//                                parabolic arc from the left controller down to
//                                the floor; release => rig.pos_xz snaps to the target.
//      - SlideHeadRelative:      Left stick => slide along the HMD's forward
//                                vector projected onto the XZ floor plane;
//                                turning the head re-steers in real time.
//      - SlideControllerRelative: Left stick => slide along the left controller's
//                                 -Z (pointing) vector projected onto XZ,
//                                 independent of head yaw.
//    Right-stick snap turn remains always-on.
//
// Comfort: we apply a circular deadzone (per Meta's input map guidance)
// and a small acceleration / deceleration ramp on the slide speed so the
// user doesn't snap from rest to full velocity in a single frame.
// ---------------------------------------------------------------------------

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
    // Priority (highest first):
    //   1. Explicit cfg.scene_json_path (set by argv[1], the top-priority
    //      source used by launchers and command-line runs).
    //   2. PTV_SCENE_JSON env var (used by the orchestrator's launch_exe,
    //      in case argv[1] wasn't passed for any reason).
    //   3. Sidecar scene.json handed in via cfg.scene_json_fallback_path
    //      (the starter-room's baked-in demo scene when double-clicking).
    if (!cfg.scene_json_path.empty()) return cfg.scene_json_path;
    if (const char* env = std::getenv("PTV_SCENE_JSON"); env && *env) {
        return env;
    }
    if (!cfg.scene_json_fallback_path.empty()) return cfg.scene_json_fallback_path;
    return {};
}

/** Turn scene JSON ``projects/<id>/assets/...`` paths into absolute paths next
 *  to the project directory so the runtime can open Tripo GLBs regardless of
 *  the process current working directory. */
static void resolve_project_mesh_paths(World& w, const std::string& scene_path) {
    namespace fs = std::filesystem;
    if (scene_path.empty()) return;
    std::error_code ec;
    fs::path scene = fs::absolute(fs::path(scene_path), ec);
    if (ec || !fs::is_regular_file(scene, ec)) return;
    fs::path project_dir = scene.parent_path();
    const std::string project_id = project_dir.filename().string();
    const std::string prefix = std::string("projects/") + project_id + "/";

    w.registry().view<CMesh>().each([&](auto /*ent*/, CMesh& mesh) {
        if (mesh.glb_path.empty()) return;
        fs::path p;
        fs::path raw(mesh.glb_path);
        if (raw.is_absolute()) {
            p = raw;
        } else if (mesh.glb_path.size() >= prefix.size() &&
                   mesh.glb_path.compare(0, prefix.size(), prefix) == 0) {
            p = project_dir / mesh.glb_path.substr(prefix.size());
        } else if (mesh.glb_path.find(':') == std::string::npos &&
                   !mesh.glb_path.empty() && mesh.glb_path[0] != '/' &&
                   mesh.glb_path[0] != '\\') {
            p = project_dir / mesh.glb_path;
        } else {
            return;
        }
        if (fs::exists(p, ec)) {
            mesh.glb_path = fs::absolute(p, ec).string();
        } else {
            PTV_WARN("GLB not found (entity mesh): {}", p.string());
        }
    });
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

// ---------------------------------------------------------------------------
// Locomotion menu + teleport aim helpers
//
// The menu is rendered as hand-authored 3D primitives so it doesn't depend on
// a font atlas or on GLB loading. Shape + colour carry the semantics:
//
//   TELEPORT pad -> a blue ring/torus (8 cubes arranged in a circle)
//                   sitting above a pale violet backdrop pad.
//   SLIDING pad  -> a green "person mid-stride" stack on a pale violet
//                   backdrop pad. (Meta's term for thumbstick continuous
//                   motion; what most players still call "walking").
//   HEAD-RELATIVE pad      -> a yellow wide-cuboid "HMD" silhouette.
//   CONTROLLER-RELATIVE pad -> a purple vertical stick with a ball at the top.
//
// Each pad has a 0.6m wide, 0.6m tall clickable region. The confirm button
// is drawn as a bright green cube next to the highlighted pad; it pulses
// using time so the user understands it's the action target.
// ---------------------------------------------------------------------------

static void push_cube(std::vector<Renderer::DrawItem>& out,
                      const Mat4& parent,
                      const Vec3& local_pos,
                      const Vec3& local_scale,
                      const Vec4& color,
                      float material_id = 1.0f) {
    Renderer::DrawItem item;
    Mat4 local = glm::translate(Mat4(1.0f), local_pos) *
                 glm::scale(Mat4(1.0f), local_scale);
    item.model = parent * local;
    item.color = color;
    item.material_id = material_id;
    out.push_back(item);
}

// Build a rotation matrix that faces +Z along ``fwd`` in the XZ plane.
static Mat4 yaw_only_matrix(const Vec3& pos, float yaw_rad) {
    Mat4 m = glm::translate(Mat4(1.0f), pos);
    return glm::rotate(m, yaw_rad, Vec3(0, 1, 0));
}

// Draw a teleport / walking option pad at ``local_offset`` in the menu frame.
// ``icon`` chooses the 3D icon variant; returns the hit-test bounding center
// in world space via ``out_world_center`` so click detection can reuse it.
static void push_menu_pad(std::vector<Renderer::DrawItem>& out,
                          const Mat4& menu_world,
                          const Vec3& local_offset,
                          int icon,
                          bool highlighted,
                          Vec3* out_world_center) {
    const Vec4 backdrop_idle{0.18f, 0.14f, 0.28f, 1.0f};   // muted violet
    const Vec4 backdrop_hi  {0.42f, 0.30f, 0.62f, 1.0f};   // vivid violet
    const Vec4 bd = highlighted ? backdrop_hi : backdrop_idle;
    // Backdrop quad (flat box).
    push_cube(out, menu_world, local_offset + Vec3(0, 0, 0),
              Vec3(0.30f, 0.30f, 0.02f), bd);

    // Icon sits just in front of the backdrop so it's visible from the user.
    const Vec3 icon_origin = local_offset + Vec3(0, 0, 0.04f);

    if (icon == 0) {
        // TELEPORT: blue "portal" torus (8 cubes in a ring).
        const Vec4 blue{0.30f, 0.65f, 1.00f, 1.0f};
        for (int i = 0; i < 8; ++i) {
            float a = static_cast<float>(i) * (6.2831853f / 8.0f);
            Vec3 p = icon_origin + Vec3(0.10f * std::cos(a),
                                         0.10f * std::sin(a) + 0.02f,
                                         0.0f);
            push_cube(out, menu_world, p, Vec3(0.035f, 0.035f, 0.035f), blue);
        }
    } else if (icon == 1) {
        // SLIDING: simple green person (head + torso + 2 legs) -- Meta's
        // term for thumbstick continuous motion.
        const Vec4 green{0.35f, 0.85f, 0.45f, 1.0f};
        push_cube(out, menu_world, icon_origin + Vec3(0.0f, 0.11f, 0.0f),
                  Vec3(0.035f, 0.035f, 0.035f), green); // head
        push_cube(out, menu_world, icon_origin + Vec3(0.0f, 0.03f, 0.0f),
                  Vec3(0.05f, 0.07f, 0.03f), green);    // torso
        push_cube(out, menu_world, icon_origin + Vec3(-0.025f, -0.08f, 0.0f),
                  Vec3(0.022f, 0.09f, 0.03f), green);   // left leg
        push_cube(out, menu_world, icon_origin + Vec3( 0.025f, -0.08f, 0.0f),
                  Vec3(0.022f, 0.09f, 0.03f), green);   // right leg
    } else if (icon == 2) {
        // HEAD-based (VR headset silhouette): wide yellow cuboid.
        const Vec4 yellow{1.00f, 0.86f, 0.25f, 1.0f};
        push_cube(out, menu_world, icon_origin + Vec3(0.0f, 0.02f, 0.0f),
                  Vec3(0.13f, 0.06f, 0.05f), yellow);  // visor
        push_cube(out, menu_world, icon_origin + Vec3(-0.11f, 0.015f, 0.0f),
                  Vec3(0.015f, 0.02f, 0.04f), yellow); // strap L
        push_cube(out, menu_world, icon_origin + Vec3( 0.11f, 0.015f, 0.0f),
                  Vec3(0.015f, 0.02f, 0.04f), yellow); // strap R
    } else if (icon == 3) {
        // JOYSTICK-based: purple stick with ball on top.
        const Vec4 purple{0.70f, 0.40f, 1.00f, 1.0f};
        push_cube(out, menu_world, icon_origin + Vec3(0.0f, -0.06f, 0.0f),
                  Vec3(0.09f, 0.03f, 0.04f), purple);  // base
        push_cube(out, menu_world, icon_origin + Vec3(0.0f, 0.01f, 0.0f),
                  Vec3(0.018f, 0.09f, 0.018f), purple); // stem
        push_cube(out, menu_world, icon_origin + Vec3(0.0f, 0.08f, 0.0f),
                  Vec3(0.04f, 0.04f, 0.04f), purple);   // ball
    }

    // Cache the world-space center for hit testing.
    if (out_world_center) {
        Vec4 p = menu_world * Vec4(local_offset, 1.0f);
        *out_world_center = Vec3(p);
    }
}

// Push a parabolic teleport arc using a sequence of thin cyan cubes. One
// cube per 20cm of arc keeps the draw-item count reasonable.
static void push_arc(std::vector<Renderer::DrawItem>& out,
                     const std::vector<Vec3>& pts,
                     const Vec4& color) {
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        Vec3 a = pts[i];
        Vec3 b = pts[i + 1];
        Vec3 mid = (a + b) * 0.5f;
        Vec3 d = b - a;
        float len = glm::length(d);
        if (len < 1e-4f) continue;
        Vec3 dir = d / len;
        // Build a yaw rotation so the cube's local Z aligns with ``dir``.
        float yaw = std::atan2(dir.x, dir.z);
        Mat4 m = glm::translate(Mat4(1.0f), mid);
        m = glm::rotate(m, yaw, Vec3(0, 1, 0));
        m = glm::scale(m, Vec3(0.015f, 0.015f, len * 0.5f));
        Renderer::DrawItem di;
        di.model = m;
        di.color = color;
        di.material_id = 3.0f;  // emissive hint for renderer if it chooses to
        out.push_back(di);
    }
}

// Compute a parabolic teleport arc from ``origin`` along ``forward`` with the
// given launch speed and gravity. Returns the ground intersection (y = 0) in
// ``out_target`` and fills ``out_points`` with sampled world-space points.
// Returns true if the arc hits the floor within ``max_horizontal_m``.
static bool compute_teleport_arc(const Vec3& origin,
                                 const Vec3& forward,
                                 float launch_speed,
                                 float gravity,
                                 float floor_y,
                                 float max_horizontal_m,
                                 std::vector<Vec3>& out_points,
                                 Vec3& out_target) {
    out_points.clear();
    Vec3 v0 = forward * launch_speed;

    // Sample the arc at ~10ms steps until it hits the floor or exits the range.
    const float dt = 0.033f;
    const int max_steps = 240;   // ~8 seconds of flight, plenty.

    Vec3 pos = origin;
    Vec3 vel = v0;
    out_points.push_back(pos);
    for (int i = 0; i < max_steps; ++i) {
        Vec3 prev = pos;
        vel.y -= gravity * dt;
        pos += vel * dt;
        out_points.push_back(pos);

        if (pos.y <= floor_y && prev.y > floor_y) {
            // Linearly interpolate the crossing point.
            float t = (prev.y - floor_y) / (prev.y - pos.y);
            Vec3 hit = prev + (pos - prev) * t;
            hit.y = floor_y;
            out_points.back() = hit;
            Vec3 horiz = hit - origin;
            horiz.y = 0.0f;
            if (glm::length(horiz) > max_horizontal_m) return false;
            out_target = hit;
            return true;
        }
        if (glm::length(Vec3(pos.x - origin.x, 0, pos.z - origin.z)) >
            max_horizontal_m + 1.0f) {
            break;
        }
    }
    return false;
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
        // Right-stick snap turn is always available (independent of
        // locomotion mode, doesn't conflict with teleport aim).
        // ------------------------------------------------------------
        {
            constexpr float kTurnDeadzone = 0.7f;

            rig_.snap_cooldown_s =
                std::max(0.0f, rig_.snap_cooldown_s - dt);
            const float rx = input.thumbstick[1].x;
            if (rig_.snap_cooldown_s <= 0.0f) {
                if (rx > kTurnDeadzone) {
                    rig_.yaw_rad -= snap_turn_degrees_ * 3.14159265f / 180.0f;
                    rig_.snap_cooldown_s = snap_turn_cooldown_s_;
                } else if (rx < -kTurnDeadzone) {
                    rig_.yaw_rad += snap_turn_degrees_ * 3.14159265f / 180.0f;
                    rig_.snap_cooldown_s = snap_turn_cooldown_s_;
                }
            }
        }

        // Menu debounce timer ticks regardless of page so the first
        // frames after a page flip don't instantly consume the trigger.
        menu_.confirm_cooldown_s =
            std::max(0.0f, menu_.confirm_cooldown_s - dt);

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
        // Startup locomotion menu.
        //
        // While the menu is open no locomotion fires (otherwise the user
        // could drift off the first panel while choosing). Confirm is
        // edge-triggered on the LEFT trigger so the user can aim with
        // the left hand and commit without changing grip.
        // ------------------------------------------------------------
        const bool menu_open =
            locomotion_menu_enabled_ && (menu_.page != MenuPage::Closed);

        // Circular deadzone per Meta's locomotion input-map guidance:
        // we measure the *magnitude* of the stick vector and apply a single
        // dead radius, then re-scale the surviving vector. A per-axis
        // deadzone (the previous behaviour) creates a square dead zone that
        // can produce strafing while pushing diagonally near rest -- exactly
        // the comfort issue Meta calls out. 0.15 keeps drift at bay on the
        // Quest 3 Touch Plus thumbsticks without eating intentional motion.
        constexpr float kStickDeadRadius = 0.15f;
        float lx = 0.0f;
        float ly = 0.0f;
        float left_mag = 0.0f;
        {
            const float lxr = input.thumbstick[0].x;
            const float lyr = input.thumbstick[0].y;
            const float raw_mag = std::sqrt(lxr * lxr + lyr * lyr);
            if (raw_mag > kStickDeadRadius) {
                // Re-scale so the surviving range is [0, 1] linearly. This
                // matches Unity XR Toolkit / Meta first-party samples.
                const float scaled =
                    (raw_mag - kStickDeadRadius) / (1.0f - kStickDeadRadius);
                const float s = std::min(scaled, 1.0f) / raw_mag;
                lx = lxr * s;
                ly = lyr * s;
                left_mag = std::min(scaled, 1.0f);
            }
        }

        if (menu_open) {
            // Anchor the panel the first time we show it so it appears in
            // front of the user's spawn pose (don't re-anchor on page
            // flips -- that would jerk the panel around mid-decision).
            if (!menu_.anchor_set) {
                menu_.world_anchor =
                    Vec3(rig_.pos_x + 0.0f, 1.55f, rig_.pos_z - 1.2f);
                menu_.yaw_rad = rig_.yaw_rad;
                menu_.anchor_set = true;
            }

            // Hit-test: shoot a ray from the left controller forward,
            // pick whichever pad's world center is closest to that ray
            // within 0.45m, and mark it highlighted.
            menu_.highlighted = -1;
            if (hand_valid[0]) {
                const Mat4 menu_world =
                    yaw_only_matrix(menu_.world_anchor, menu_.yaw_rad);
                Vec3 pad_a, pad_b;
                // Compute world centers for the two pads on this page.
                Vec4 a4 = menu_world * Vec4(-0.35f, 0.0f, 0.0f, 1.0f);
                Vec4 b4 = menu_world * Vec4( 0.35f, 0.0f, 0.0f, 1.0f);
                pad_a = Vec3(a4);
                pad_b = Vec3(b4);

                Vec3 ro = hand_world_pos[0];
                Vec3 rd = hand_world_rot[0] * Vec3(0, 0, -1); // -Z forward
                rd = glm::normalize(rd);

                auto ray_dist = [&](const Vec3& p) {
                    Vec3 v = p - ro;
                    float t = glm::dot(v, rd);
                    if (t < 0) return 1e9f;
                    Vec3 closest = ro + rd * t;
                    return glm::length(closest - p);
                };
                float da = ray_dist(pad_a);
                float db = ray_dist(pad_b);
                const float kHitRadius = 0.35f;
                if (da < kHitRadius || db < kHitRadius) {
                    menu_.highlighted = (da <= db) ? 0 : 1;
                }
            }

            // Confirm: rising edge on left trigger (values in [0,1]).
            const float trig = input.trigger[0];
            const bool trig_edge = (menu_.prev_left_trigger < 0.5f) &&
                                   (trig >= 0.5f);
            if (trig_edge && menu_.highlighted >= 0 &&
                menu_.confirm_cooldown_s <= 0.0f) {
                if (menu_.page == MenuPage::Root) {
                    if (menu_.highlighted == 0) {
                        locomotion_ = LocomotionMode::Teleport;
                        menu_.page = MenuPage::Closed;
                        PTV_INFO("Locomotion: Teleport selected.");
                    } else {
                        menu_.page = MenuPage::SlidingSubmenu;
                        menu_.confirm_cooldown_s = 0.5f;
                        PTV_INFO("Menu: sliding submenu.");
                    }
                } else if (menu_.page == MenuPage::SlidingSubmenu) {
                    locomotion_ = (menu_.highlighted == 0)
                                      ? LocomotionMode::SlideHeadRelative
                                      : LocomotionMode::SlideControllerRelative;
                    menu_.page = MenuPage::Closed;
                    PTV_INFO(
                        "Locomotion: Sliding ({}) selected.",
                        (locomotion_ == LocomotionMode::SlideHeadRelative)
                            ? "head-relative"
                            : "controller-relative");
                }
            }
            menu_.prev_left_trigger = trig;
        }

        // ------------------------------------------------------------
        // Left-stick locomotion (mode-aware). Disabled while the menu is
        // open.
        //   * Teleport:                aim-and-commit driven by stick magnitude.
        //   * SlideHeadRelative:       direction = HMD forward on XZ; the
        //                              latest per-frame HMD pose feeds
        //                              ``last_head_forward_xz_`` so turning
        //                              the head re-steers smoothly.
        //   * SlideControllerRelative: direction = left controller's -Z aim
        //                              on XZ; head-independent.
        // We cap target speed at ~1.5 m/s (Meta's recommended walking pace)
        // and lerp ``current_slide_speed_mps_`` toward it so engagement and
        // disengagement are smooth (vection comfort).
        // ------------------------------------------------------------
        teleport_.aiming = false;
        teleport_.have_target = false;
        teleport_.arc_points.clear();

        if (!menu_open) {
            const float kSlideMaxSpeed = slide_max_speed_mps_;
            const float kSlideAccelTau = slide_accel_tau_s_;
            const float kSlideDecelTau = slide_decel_tau_s_;

            if (locomotion_ == LocomotionMode::Teleport) {
                const float kAimThreshold = 0.5f;    // push to aim
                const float kReleaseThreshold = 0.1f;// release to commit

                static bool was_aiming_prev = false;
                static Vec3 last_target{0.0f};
                static bool last_have_target = false;

                if (hand_valid[0] && left_mag > kAimThreshold) {
                    Vec3 origin = hand_world_pos[0];
                    // Fire the arc slightly downward of the controller's
                    // forward vector so small tilts produce a comfortable
                    // medium-range arc (~3-6m) rather than flying off.
                    Vec3 forward = hand_world_rot[0] * Vec3(0, 0, -1);
                    forward.y -= 0.15f;
                    forward = glm::normalize(forward);

                    Vec3 tgt;
                    bool ok = compute_teleport_arc(
                        origin, forward,
                        /*launch_speed*/ teleport_launch_speed_mps_,
                        /*gravity*/ 9.8f,
                        /*floor_y*/ teleport_floor_y_,
                        /*max_horizontal_m*/ teleport_max_horizontal_m_,
                        teleport_.arc_points, tgt);
                    teleport_.aiming = true;
                    teleport_.have_target = ok;
                    if (ok) teleport_.target = tgt;
                    last_have_target = ok;
                    last_target = tgt;
                    was_aiming_prev = true;
                } else if (was_aiming_prev && left_mag < kReleaseThreshold) {
                    // Release edge: commit if we had a valid target.
                    if (last_have_target) {
                        rig_.pos_x = last_target.x;
                        rig_.pos_z = last_target.z;
                        PTV_INFO("Teleport -> ({:.2f}, {:.2f})",
                                 last_target.x, last_target.z);
                    }
                    was_aiming_prev = false;
                    last_have_target = false;
                }
            } else {
                // Sliding locomotion on the XZ plane. OpenXR thumbstick Y is
                // **negative** when the user pushes the stick forward
                // (away from the body) on Quest / Quest Link, matching
                // Meta's input-map docs and first-party samples.
                const float forward_axis = -ly;
                const float strafe_axis = lx;

                Vec3 fwd(0.0f);
                Vec3 right(0.0f);
                bool have_basis = false;

                if (locomotion_ == LocomotionMode::SlideHeadRelative) {
                    // Head-Relative sliding: direction = HMD forward
                    // projected onto the XZ floor plane. Updated each
                    // frame from `last_head_forward_xz_` so turning
                    // the head re-steers in real time.
                    fwd = last_head_forward_xz_;
                    const float flen = glm::length(fwd);
                    if (flen > 1e-5f) {
                        fwd /= flen;
                        right = glm::normalize(
                            glm::cross(Vec3(0.0f, 1.0f, 0.0f), fwd));
                        have_basis = true;
                    }
                } else if (locomotion_ == LocomotionMode::SlideControllerRelative) {
                    // Controller-Relative sliding: direction = the left
                    // controller's pointing vector (-Z aim) projected
                    // onto XZ. Head yaw plays no role.
                    if (hand_valid[0]) {
                        Vec3 aim = hand_world_rot[0] * Vec3(0.0f, 0.0f, -1.0f);
                        fwd = Vec3(aim.x, 0.0f, aim.z);
                        const float flen = glm::length(fwd);
                        if (flen > 1e-5f) {
                            fwd /= flen;
                            right = glm::normalize(
                                glm::cross(Vec3(0.0f, 1.0f, 0.0f), fwd));
                            have_basis = true;
                        }
                    }
                }

                // Comfort ramp: lerp current speed toward
                //   target = stick_magnitude * max_speed
                // with different time-constants for accel vs decel.
                const float target_speed =
                    have_basis ? (left_mag * kSlideMaxSpeed) : 0.0f;
                const float tau =
                    (target_speed > current_slide_speed_mps_) ? kSlideAccelTau
                                                              : kSlideDecelTau;
                // Exponential approach: x += (target - x) * (1 - exp(-dt/tau)).
                const float k = 1.0f - std::exp(-dt / std::max(tau, 1e-3f));
                current_slide_speed_mps_ +=
                    (target_speed - current_slide_speed_mps_) * k;

                if (have_basis && current_slide_speed_mps_ > 1e-4f &&
                    left_mag > 0.0f) {
                    // Re-normalise the (forward,strafe) intent so the
                    // resulting velocity magnitude equals the ramped speed,
                    // independent of stick magnitude (which is already
                    // baked into target_speed -> current_slide_speed_mps_).
                    const float inv_mag =
                        (left_mag > 1e-5f) ? (1.0f / left_mag) : 0.0f;
                    const float fa = forward_axis * inv_mag;
                    const float sa = strafe_axis * inv_mag;
                    const float vx = fwd.x * fa + right.x * sa;
                    const float vz = fwd.z * fa + right.z * sa;
                    rig_.pos_x += vx * current_slide_speed_mps_ * dt;
                    rig_.pos_z += vz * current_slide_speed_mps_ * dt;
                }
            }
        } else {
            // Menu open -> bleed any residual slide speed so we don't
            // resume motion the moment the user confirms.
            current_slide_speed_mps_ = 0.0f;
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

            // -------- Locomotion menu 3D panel --------
            if (locomotion_menu_enabled_ && menu_.page != MenuPage::Closed) {
                const Mat4 menu_world =
                    yaw_only_matrix(menu_.world_anchor, menu_.yaw_rad);

                // Title bar (cyan slab above the pads) acts as a visual
                // "sign" even without text rendering.
                const Vec4 title_color =
                    (menu_.page == MenuPage::Root)
                        ? Vec4(0.25f, 0.85f, 1.00f, 1.0f)    // cyan = root
                        : Vec4(1.00f, 0.55f, 0.85f, 1.0f);   // pink = sliding
                push_cube(items, menu_world, Vec3(0.0f, 0.28f, 0.0f),
                          Vec3(0.80f, 0.07f, 0.03f), title_color);

                int icon_left  = (menu_.page == MenuPage::Root) ? 0 : 2;
                int icon_right = (menu_.page == MenuPage::Root) ? 1 : 3;
                Vec3 wc_a, wc_b;
                push_menu_pad(items, menu_world, Vec3(-0.35f, 0.0f, 0.0f),
                              icon_left,  menu_.highlighted == 0, &wc_a);
                push_menu_pad(items, menu_world, Vec3( 0.35f, 0.0f, 0.0f),
                              icon_right, menu_.highlighted == 1, &wc_b);

                // Pulsing green confirm button next to whichever pad is
                // currently targeted. It's only visible when something
                // is highlighted, signalling "pull trigger to confirm".
                if (menu_.highlighted >= 0) {
                    using clk = std::chrono::steady_clock;
                    static const auto t0 = clk::now();
                    float t = std::chrono::duration<float>(clk::now() - t0).count();
                    float pulse = 0.70f + 0.30f * std::sin(t * 4.0f);
                    Vec3 pos = (menu_.highlighted == 0)
                                   ? Vec3(-0.35f, -0.22f, 0.0f)
                                   : Vec3( 0.35f, -0.22f, 0.0f);
                    push_cube(items, menu_world, pos,
                              Vec3(0.10f, 0.06f, 0.02f),
                              Vec4(0.15f * pulse, 0.95f * pulse,
                                   0.25f * pulse, 1.0f),
                              3.0f);
                }
            }

            // -------- Teleport aim arc + landing marker --------
            if (teleport_.aiming && teleport_.arc_points.size() >= 2) {
                const Vec4 arc_color = teleport_.have_target
                                           ? Vec4(0.30f, 0.75f, 1.00f, 1.0f)
                                           : Vec4(0.60f, 0.30f, 0.30f, 1.0f);
                push_arc(items, teleport_.arc_points, arc_color);
                if (teleport_.have_target) {
                    // Landing disc: flat cyan quad at the target point,
                    // sized like a 0.8m circle (approximated by a box).
                    Mat4 m = glm::translate(Mat4(1.0f), teleport_.target +
                                                          Vec3(0, 0.01f, 0));
                    m = glm::scale(m, Vec3(0.4f, 0.01f, 0.4f));
                    Renderer::DrawItem di;
                    di.model = m;
                    di.color = Vec4(0.30f, 0.85f, 1.00f, 1.0f);
                    di.material_id = 3.0f;
                    items.push_back(di);
                }
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
                    // Cache HMD forward on the XZ plane for head-referenced
                    // walking (OpenXR view space: -Z is where the user looks).
                    Mat4 w_from_head =
                        rig_mat * xr_pose_to_matrix(eyes[0].pose);
                    Vec3 head_fwd = Vec3(w_from_head * Vec4(0, 0, -1, 0));
                    Vec3 flat(head_fwd.x, 0.0f, head_fwd.z);
                    const float flen = glm::length(flat);
                    if (flen > 1e-4f) {
                        last_head_forward_xz_ = flat / flen;
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
        // Stage / rig origin from authored ``player.spawn`` so generated games
        // can drop the user onto a sensible floor height (e.g. above a GLB).
        rig_.pos_x = scene_.spawn.position.x;
        rig_.pos_y = scene_.spawn.position.y;
        rig_.pos_z = scene_.spawn.position.z;
        {
            Vec3 f =
                glm::mat3_cast(scene_.spawn.rotation) * Vec3(0.0f, 0.0f, -1.0f);
            const float fl = glm::length(Vec3(f.x, 0.0f, f.z));
            if (fl > 1e-5f) {
                rig_.yaw_rad = std::atan2(f.x, -f.z);
            }
        }
        ApplySceneToWorld(scene_, world_);
        resolve_project_mesh_paths(world_, scene_path_);
        physics_.rebuild_from_world(world_);

        locomotion_menu_enabled_ = scene_.locomotion_menu_enabled;
        slide_max_speed_mps_ = scene_.slide_max_speed_mps;
        slide_accel_tau_s_ = scene_.slide_accel_tau_s;
        slide_decel_tau_s_ = scene_.slide_decel_tau_s;
        teleport_launch_speed_mps_ = scene_.teleport_launch_speed_mps;
        teleport_max_horizontal_m_ = scene_.teleport_max_horizontal_m;
        teleport_floor_y_ = scene_.teleport_floor_y;
        snap_turn_degrees_ = scene_.snap_turn_degrees;
        snap_turn_cooldown_s_ = scene_.snap_turn_cooldown_s;

        if (!locomotion_menu_enabled_) {
            menu_.page = MenuPage::Closed;
            auto lower = [](std::string s) {
                for (char& c : s) {
                    c = static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                }
                return s;
            };
            const std::string m = lower(scene_.default_locomotion_mode);
            if (m.find("controller") != std::string::npos) {
                locomotion_ = LocomotionMode::SlideControllerRelative;
            } else if (m.find("head") != std::string::npos ||
                       m == "slide_head_relative") {
                locomotion_ = LocomotionMode::SlideHeadRelative;
            } else if (m.find("teleport") != std::string::npos) {
                locomotion_ = LocomotionMode::Teleport;
            } else if (!scene_.default_locomotion_mode.empty()) {
                locomotion_ = LocomotionMode::Teleport;
            } else if (scene_.locomotion == "mixed" ||
                       scene_.locomotion == "smooth") {
                locomotion_ = LocomotionMode::SlideHeadRelative;
            } else if (scene_.locomotion == "teleport" ||
                       scene_.locomotion == "stationary") {
                locomotion_ = LocomotionMode::Teleport;
            } else {
                locomotion_ = LocomotionMode::SlideHeadRelative;
            }
        }
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
