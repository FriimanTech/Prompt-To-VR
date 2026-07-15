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
//
// OpenXR reference spaces (https://registry.khronos.org/OpenXR/specs/1.1/
// html/xrspec.html#reference-spaces):
//   * STAGE  -- the play-space; ground-plane origin. Hand poses, view
//               poses and the rig translation are all expressed here.
//   * VIEW   -- centered on the HMD; tracks the head pose. We locate
//               this against STAGE every frame in ``XrRuntime::sync_input``
//               and surface the result as ``InputState::head``. Locomotion
//               consumes that single fresh pose before any slide math runs,
//               so head-relative movement re-steers immediately when the
//               user turns their head -- there is no fixed world-space
//               forward, no spawn-yaw fallback, and no stale per-frame
//               cache anywhere in the slide path. Same applies to the
//               left controller for controller-relative slide.
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

// Locate the repository's ``3D_Objects/`` directory by walking up from
// the running executable. Returns an empty string if we can't find one
// within ten parents (in which case the locomotion menu falls back to
// procedural icons). Windows-only (matches the rest of the runtime).
static std::string find_3d_objects_dir() {
    namespace fs = std::filesystem;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    fs::path dir = fs::path(std::wstring(buf, n)).parent_path();
    std::error_code ec;
    for (int i = 0; i < 12 && !dir.empty(); ++i) {
        fs::path candidate = dir / "3D_Objects";
        if (fs::is_directory(candidate, ec)) {
            return fs::absolute(candidate, ec).string();
        }
        const fs::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Procedural hand visual: palm + wrist stub.
//
// The mesh is designed in the OpenXR /grip/pose local frame, per the
// 1.1 spec ("Standard pose identifiers"):
//
//   -Z = "forward" (sword blade direction, from pinky to index when
//         the user makes a fist around the controller)
//   +Z = "backward" (toward the user's forearm / wrist)
//   +Y = orthogonal-up (right-hand rule of +X and +Z)
//   +X = palm normal (away from palm for LEFT hand, INTO palm for
//         RIGHT hand) -- handedness-dependent, so the mesh is kept
//         symmetric in X to avoid per-hand mirroring.
//
// We deliberately render only a palm + wrist stub (no thumb / no
// finger detail). A protruding thumb would have to flip sign on +X
// per hand, which would mean rebuilding the mesh twice; the current
// reusable-symmetric design ships one mesh that reads correctly for
// both controllers.
//
// An older revision of this function had thumb at +Z and wrist at -Z,
// which is the OPPOSITE of the OpenXR convention. The visible symptom
// was that the hand model appeared to point "diagonally upward" when
// the user held the controller flat -- because Touch's /grip pose
// has -Z aligned with the angled grip handle (~30 deg above horizontal
// when the body is held flat), so the mesh's "forward" rendered as
// up-and-forward in world space. Fixing the axis convention puts the
// fingertips on the user's actual hand-forward direction and the
// wrist behind, matching anatomy.
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

    // Palm / closed-fist body: oblong slab centred at the grip origin
    // (palm centroid). Long axis is Z (wrist <-> fingertips), shorter
    // along Y (palm <-> back-of-hand) and X (pinky <-> index direction).
    push(Vec3(0.0f, 0.0f, 0.0f),
         Vec3(0.050f, 0.045f, 0.10f));

    // Wrist / forearm stub: extends +Z (toward the forearm, opposite
    // of the fingertips) so the hand looks anchored to something
    // instead of floating as a free palm.
    push(Vec3(0.0f, 0.0f, +0.085f),
         Vec3(0.040f, 0.040f, 0.07f));
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
//
// ``glb_slot`` is the renderer's pre-loaded mesh slot for the icon's 3D
// model; pass -1 to fall back to the procedural cube icon selected by
// ``procedural_icon`` (0=teleport portal, 1=person, 2=headset, 3=joystick).
// Returns the hit-test bounding center in world space via
// ``out_world_center`` so click detection can reuse it.
static void push_menu_pad(std::vector<Renderer::DrawItem>& out,
                          const Mat4& menu_world,
                          const Vec3& local_offset,
                          int glb_slot,
                          int procedural_icon,
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

    // Preferred path: the user supplied a labelled 3D model (Walking /
    // Teleport / Head-Relative / Controller-Relative GLB). The renderer
    // already pre-scaled the GLB so its longest axis is ``kIconScale`` (set
    // at load time in Application::initialize), so all four icons fit a
    // 0.30m pad consistently.
    if (glb_slot >= 0) {
        Renderer::DrawItem di{};
        Mat4 local = glm::translate(Mat4(1.0f), icon_origin);
        di.model = menu_world * local;
        di.mesh_slot = glb_slot;
        // Slight highlight pop when the user is aiming at this pad so the
        // selection cue is unmistakable in VR.
        di.color = highlighted
                       ? Vec4(1.10f, 1.10f, 1.10f, 1.0f)
                       : Vec4(0.95f, 0.95f, 0.95f, 1.0f);
        di.material_id = 0.0f;
        out.push_back(di);
        if (out_world_center) {
            Vec4 p = menu_world * Vec4(local_offset, 1.0f);
            *out_world_center = Vec3(p);
        }
        return;
    }

    // Procedural fallback (unchanged from earlier behaviour) -- only used
    // if the GLB couldn't be loaded (e.g. running from a build that no
    // longer ships the 3D_Objects/ folder).
    const int icon = procedural_icon;
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
                                 Vec3& out_target,
                                 const PhysicsWorld* physics = nullptr) {
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

        // Prefer a real physics hit against the generated environment so the
        // player can teleport onto ground/platforms/terrain at any height.
        if (physics != nullptr) {
            Vec3 seg = pos - prev;
            float seg_len = glm::length(seg);
            Vec3 hit, normal;
            if (seg_len > 1e-5f &&
                physics->raycast(prev, seg, seg_len, hit, normal)) {
                out_points.back() = hit;
                Vec3 horiz = hit - origin;
                horiz.y = 0.0f;
                if (glm::length(horiz) > max_horizontal_m) return false;
                // Reject near-vertical surfaces (walls) as landing spots.
                if (normal.y < 0.4f) return false;
                out_target = hit;
                return true;
            }
        }

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

        // ------------------------------------------------------------
        // OpenXR frame timing.
        //
        // ``begin_xr_frame()`` calls xrWaitFrame + xrBeginFrame and
        // updates ``last_predicted_display_time_`` for THIS frame. We
        // do this BEFORE ``sync_input`` so the head/hand poses we
        // locate against the stage are at the *current* frame's
        // predicted display time, not the previous frame's.
        //
        // Returns false when the runtime asked us to skip rendering
        // (in which case it has already submitted an empty end-frame
        // and we just bypass the render block below) or the session
        // is not running yet. We still run sync_input afterwards so
        // session-state edges (focus, ready) get exercised, but the
        // values will simply be stale identity until the runtime is
        // ready.
        const bool xr_should_render = xr_.begin_xr_frame();

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
        //
        // ``hand_*`` come from the OpenXR /grip pose (palm-centric, used
        // for visualization, grab detection, kinematic follow).
        // ``aim_*`` come from the OpenXR /aim pose (controller-body-
        // forward, used for slide direction, teleport arc, ray casts).
        // The two are different because Touch controllers have an angled
        // grip handle -- /grip's -Z is along the hand, /aim's -Z is
        // along the controller body. Locomotion that asks "where is the
        // controller pointing?" must read /aim, not /grip, or pushing
        // the stick forward feels like the controller is angled up.
        const bool hand_valid[2] = {input.hand_left_valid, input.hand_right_valid};
        const XrPosef hand_poses[2] = {input.hand_left.pose, input.hand_right.pose};
        const bool aim_valid[2]  = {input.aim_left_valid, input.aim_right_valid};
        const XrPosef aim_poses[2] = {input.aim_left.pose, input.aim_right.pose};
        const Quat rig_yaw_quat = glm::angleAxis(rig_.yaw_rad, Vec3(0, 1, 0));
        Mat4 hand_world[2]{Mat4(1.0f), Mat4(1.0f)};
        Vec3 hand_world_pos[2]{Vec3(0.0f), Vec3(0.0f)};
        Quat hand_world_rot[2]{Quat(1,0,0,0), Quat(1,0,0,0)};
        Quat aim_world_rot[2]{Quat(1,0,0,0), Quat(1,0,0,0)};
        bool aim_world_valid[2]{false, false};
        for (int h = 0; h < 2; ++h) {
            if (hand_valid[h]) {
                hand_world[h] = rig_mat * xr_pose_to_matrix(hand_poses[h]);
                hand_world_pos[h] = mat_translation(hand_world[h]);
                // Rig rotation is yaw only, so we can compose rotations directly.
                hand_world_rot[h] = rig_yaw_quat * xr_pose_to_quat(hand_poses[h]);
            }
            if (aim_valid[h]) {
                aim_world_rot[h] = rig_yaw_quat * xr_pose_to_quat(aim_poses[h]);
                aim_world_valid[h] = true;
            } else if (hand_valid[h]) {
                // Older Touch profiles or non-aim-bound controllers can
                // still slide using grip orientation as a fallback (with
                // the well-known ~30 deg upward bias). We log this once
                // so the user knows why the angle feels off.
                aim_world_rot[h] = hand_world_rot[h];
                aim_world_valid[h] = true;
            }
        }

        // ------------------------------------------------------------
        // Fresh HMD pose (this frame).
        //
        // ``input.head`` is the OpenXR VIEW reference space located
        // against the stage at the just-predicted display time -- i.e.
        // the latest tracked head pose, in the SAME coordinate system
        // as the hand poses above. We compose it through ``rig_mat``
        // to get a world-space head pose, then derive a horizontal
        // forward vector for head-relative sliding.
        //
        // We deliberately do NOT cache a stale ``last_head_forward_xz_``
        // for use by gameplay code below; we recompute every frame so
        // turning the head 180 deg immediately re-steers the slide.
        // (We still write ``last_head_forward_xz_`` /
        // ``last_head_world_pos_`` at the end of this block so other
        // systems -- the menu anchor placement, debug overlays -- have
        // a sensible value when the HMD pose was momentarily invalid.)
        // ------------------------------------------------------------
        Vec3 head_world_pos = last_head_world_pos_;
        Quat head_world_rot = Quat(1, 0, 0, 0);
        Vec3 head_forward_xz = last_head_forward_xz_;
        bool head_pose_fresh = false;
        if (input.head_valid) {
            const Mat4 head_world_mat =
                rig_mat * xr_pose_to_matrix(input.head.pose);
            head_world_pos = mat_translation(head_world_mat);
            head_world_rot =
                rig_yaw_quat * xr_pose_to_quat(input.head.pose);
            // Horizontal HMD forward (project -Z onto XZ, normalize).
            // The user's head pitch (looking up/down) is intentionally
            // discarded so head-relative slide stays parallel to the
            // virtual ground plane regardless of vertical gaze --
            // matches Meta's locomotion comfort guidance.
            const Vec3 fwd_world = head_world_rot * Vec3(0.0f, 0.0f, -1.0f);
            const Vec3 flat(fwd_world.x, 0.0f, fwd_world.z);
            const float flen = glm::length(flat);
            if (flen > 1e-4f) {
                head_forward_xz = flat / flen;
                head_pose_fresh = true;
            }
            last_head_world_pos_ = head_world_pos;
            last_head_pose_valid_ = true;
        }
        // Always mirror the freshest forward we have into the cache so
        // any consumer reading the member sees the latest value rather
        // than the pre-snap-turn one.
        if (head_pose_fresh) {
            last_head_forward_xz_ = head_forward_xz;
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
        // deadzone alone creates a square dead zone that can produce
        // strafing while pushing diagonally near rest -- exactly the
        // comfort issue Meta calls out. 0.18 keeps drift at bay on Quest
        // 3 Touch Plus thumbsticks (which have noticeable cross-axis
        // bleed after a few months of use) without eating intentional
        // motion.
        //
        // After the radial pass we apply a *cardinal snap*: when one axis
        // strongly dominates (>0.5) and the other is small (<0.18), the
        // small axis is zeroed out. This makes "push pure right" feel
        // like pure right rather than slow diagonal-right (which is what
        // the user sees when the stick has 0.10 of off-axis bleed).
        // Intentional diagonals are preserved because both axes have to
        // be small for the snap to fire.
        constexpr float kStickDeadRadius = 0.18f;
        constexpr float kAxisDominant    = 0.50f;
        constexpr float kAxisCrossSnap   = 0.18f;
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
                // Cardinal snap.
                if (std::abs(lx) > kAxisDominant && std::abs(ly) < kAxisCrossSnap) {
                    ly = 0.0f;
                } else if (std::abs(ly) > kAxisDominant &&
                           std::abs(lx) < kAxisCrossSnap) {
                    lx = 0.0f;
                }
                // Recompute magnitude after the snap so the speed-ramp
                // target uses the corrected vector length.
                left_mag = std::sqrt(lx * lx + ly * ly);
                left_mag = std::min(left_mag, 1.0f);
            }
        }

        if (menu_open) {
            // Anchor the panel the first time we show it (and have a real
            // HMD pose) so it appears at a comfortable spot in front of
            // the user's eyes. Don't re-anchor on page flips -- that
            // would jerk the panel around mid-decision.
            //
            // Per Meta Horizon "Head -> Comfort" guidance: place primary
            // UI 1.5m forward, slightly below eye level (~10-15 deg below
            // horizon) so the user's natural line of sight rests on it
            // without neck strain. At 1.5m, 10 deg below = ~0.20m drop.
            if (!menu_.anchor_set && last_head_pose_valid_) {
                // Anchor relative to the freshest head pose we have.
                const Vec3 head = head_pose_fresh ? head_world_pos
                                                  : last_head_world_pos_;
                const Vec3 fwd  = head_pose_fresh ? head_forward_xz
                                                  : last_head_forward_xz_;
                constexpr float kMenuDistance      = 1.5f;
                constexpr float kMenuBelowEyeMeters = 0.20f;
                menu_.world_anchor = Vec3(
                    head.x + fwd.x * kMenuDistance,
                    head.y - kMenuBelowEyeMeters,
                    head.z + fwd.z * kMenuDistance);
                // Rotate so the panel's local +Z (where the icons sit)
                // points back toward the user. yaw_only_matrix(pos, y)
                // rotates +Z by ``y`` around world Y; we want
                // R(y)*(0,0,1) == -fwd, hence atan2(-fwd.x, -fwd.z).
                menu_.yaw_rad = std::atan2(-fwd.x, -fwd.z);
                menu_.anchor_set = true;
                PTV_INFO(
                    "Locomotion menu anchored at ({:.2f}, {:.2f}, {:.2f}) "
                    "{:.2f}m forward of HMD eye (y={:.2f}m).",
                    menu_.world_anchor.x, menu_.world_anchor.y,
                    menu_.world_anchor.z, kMenuDistance, head.y);
            }

            // Hit-test: shoot a ray from EACH controller (the user can
            // point with either hand) and pick whichever pad/hand pair
            // has the closest distance below ``kHitRadius``. ``active_hand``
            // remembers which controller drove the highlight so we can
            // show the right confirmation hint AND so a stray trigger
            // press from the *other* hand doesn't false-fire.
            menu_.highlighted = -1;
            menu_.active_hand = -1;
            {
                const Mat4 menu_world =
                    yaw_only_matrix(menu_.world_anchor, menu_.yaw_rad);
                // Compute world centers for the two pads on this page.
                Vec4 a4 = menu_world * Vec4(-0.35f, 0.0f, 0.0f, 1.0f);
                Vec4 b4 = menu_world * Vec4( 0.35f, 0.0f, 0.0f, 1.0f);
                const Vec3 pad_a(a4);
                const Vec3 pad_b(b4);
                const float kHitRadius = 0.35f;
                float best_dist = kHitRadius;

                for (int h = 0; h < 2; ++h) {
                    if (!hand_valid[h] || !aim_world_valid[h]) continue;
                    Vec3 ro = hand_world_pos[h];
                    // Aim along the controller body (/aim/pose -Z), not
                    // the angled grip handle (/grip/pose -Z), so the
                    // user points where the controller visually points.
                    Vec3 rd = aim_world_rot[h] * Vec3(0, 0, -1);
                    rd = glm::normalize(rd);

                    auto ray_dist = [&](const Vec3& p) {
                        Vec3 v = p - ro;
                        float t = glm::dot(v, rd);
                        if (t < 0) return 1e9f;
                        Vec3 closest = ro + rd * t;
                        return glm::length(closest - p);
                    };
                    const float da = ray_dist(pad_a);
                    const float db = ray_dist(pad_b);
                    const int hi = (da <= db) ? 0 : 1;
                    const float d = (hi == 0) ? da : db;
                    if (d < best_dist) {
                        best_dist = d;
                        menu_.highlighted = hi;
                        menu_.active_hand = h;
                    }
                }
            }

            // Confirm: rising edge on EITHER trigger. We accept any hand's
            // trigger so both controllers feel equally responsive (the
            // visual highlight already tells the user which pad will fire).
            for (int h = 0; h < 2; ++h) {
                const float trig = input.trigger[h];
                const bool trig_edge =
                    (menu_.prev_trigger[h] < 0.5f) && (trig >= 0.5f);
                if (trig_edge && menu_.highlighted >= 0 &&
                    menu_.confirm_cooldown_s <= 0.0f) {
                    if (menu_.page == MenuPage::Root) {
                        // Root page layout: LEFT pad = Walking (opens the
                        // sliding-style sub-menu), RIGHT pad = Teleport.
                        if (menu_.highlighted == 0) {
                            menu_.page = MenuPage::SlidingSubmenu;
                            menu_.confirm_cooldown_s = 0.5f;
                            PTV_INFO("Menu: sliding submenu.");
                        } else {
                            locomotion_ = LocomotionMode::Teleport;
                            menu_.page = MenuPage::Closed;
                            PTV_INFO("Locomotion: Teleport selected.");
                        }
                    } else if (menu_.page == MenuPage::SlidingSubmenu) {
                        // Sub-menu: LEFT = Head-Relative, RIGHT = Controller-Relative.
                        locomotion_ =
                            (menu_.highlighted == 0)
                                ? LocomotionMode::SlideHeadRelative
                                : LocomotionMode::SlideControllerRelative;
                        menu_.page = MenuPage::Closed;
                        PTV_INFO(
                            "Locomotion: Sliding ({}) selected.",
                            (locomotion_ == LocomotionMode::SlideHeadRelative)
                                ? "head-relative"
                                : "controller-relative");
                    }
                    // Stop scanning the rest of the hands this frame --
                    // we already committed.
                    menu_.prev_trigger[h] = trig;
                    for (int j = h + 1; j < 2; ++j) {
                        menu_.prev_trigger[j] = input.trigger[j];
                    }
                    break;
                }
                menu_.prev_trigger[h] = trig;
            }
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

                if (hand_valid[0] && aim_world_valid[0] && left_mag > kAimThreshold) {
                    Vec3 origin = hand_world_pos[0];
                    // Fire the arc along the controller's AIM direction
                    // (where the body points) rather than the GRIP -Z
                    // axis (which is angled up because Touch's grip
                    // handle is angled). Then bias slightly downward so
                    // small tilts produce a comfortable medium-range arc
                    // (~3-6m) rather than flying off.
                    Vec3 forward = aim_world_rot[0] * Vec3(0, 0, -1);
                    forward.y -= 0.15f;
                    forward = glm::normalize(forward);

                    Vec3 tgt;
                    bool ok = compute_teleport_arc(
                        origin, forward,
                        /*launch_speed*/ teleport_launch_speed_mps_,
                        /*gravity*/ 9.8f,
                        /*floor_y*/ teleport_floor_y_,
                        /*max_horizontal_m*/ teleport_max_horizontal_m_,
                        teleport_.arc_points, tgt, &physics_);
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
                        // Land on the surface height so the player can traverse
                        // onto generated platforms / terrain, not just y=floor.
                        rig_.pos_y = last_target.y;
                        PTV_INFO("Teleport -> ({:.2f}, {:.2f}, {:.2f})",
                                 last_target.x, last_target.y, last_target.z);
                    }
                    was_aiming_prev = false;
                    last_have_target = false;
                }
            } else {
                // Sliding locomotion on the XZ plane.
                //
                // OpenXR thumbstick convention (1.1 spec, "Standard
                // input identifiers"): "+1 means all the way right for
                // x axis or all the way up for y axis." Pushing the
                // stick forward (away from the body) reads as +Y. So
                // forward_axis = +ly. (A previous build had this
                // negated, which is why pushing forward produced
                // backward motion on Quest Link.)
                const float forward_axis = ly;
                const float strafe_axis  = lx;

                // The slide-direction basis is recomputed every frame
                // from the LATEST tracked pose (HMD or left controller,
                // depending on mode). There is intentionally NO fixed
                // world forward, no spawn-yaw fallback, and no stale
                // cache: turning the head or the controller mid-stride
                // immediately re-steers movement.
                Vec3 fwd(0.0f);
                Vec3 right(0.0f);
                bool have_basis = false;

                if (locomotion_ == LocomotionMode::SlideHeadRelative) {
                    // Head-Relative sliding: forward = the HMD's current
                    // -Z direction, projected onto the XZ ground plane.
                    //
                    // We require a *real* head pose source -- either
                    // freshly located this frame (head_pose_fresh) or a
                    // cached value previously set from a real fresh
                    // pose. The cache defaults to the ZERO vector (see
                    // Application.h) so that until the runtime has
                    // delivered a single valid VIEW pose, no slide
                    // direction is available and the player simply
                    // stands still. There is intentionally no
                    // fixed-world-space fallback: that bug used to make
                    // the player slide in world -Z regardless of where
                    // they were physically looking.
                    Vec3 candidate(0.0f);
                    if (head_pose_fresh) {
                        candidate = head_forward_xz;
                    } else if (glm::length(last_head_forward_xz_) > 1e-4f) {
                        candidate = last_head_forward_xz_;
                    }
                    const float flen = glm::length(candidate);
                    if (flen > 1e-5f) {
                        fwd = candidate / flen;
                        // Right-handed Y-up: when facing fwd=-Z, the
                        // user's right is +X. The correct formula is
                        // ``cross(fwd, up)`` (not ``cross(up, fwd)``),
                        // which gave -X and silently swapped strafe.
                        right = glm::normalize(
                            glm::cross(fwd, Vec3(0.0f, 1.0f, 0.0f)));
                        have_basis = true;
                    }
                } else if (locomotion_ == LocomotionMode::SlideControllerRelative) {
                    // Controller-Relative sliding: forward = the LEFT
                    // controller's -Z in the OpenXR /aim/pose frame
                    // (controller body forward, NOT grip handle), then
                    // projected onto the XZ ground plane. Independent
                    // of head yaw -- the user can look anywhere while
                    // gliding along the controller's aim. Tilt is
                    // discarded so pointing the controller up/down
                    // doesn't move the player vertically.
                    //
                    // We use /aim instead of /grip because Quest Touch
                    // controllers have an angled grip handle: /grip's
                    // -Z is along the user's hand (~30 deg up from
                    // horizontal when held flat), while /aim's -Z is
                    // along the controller body (where the user
                    // intuitively expects to "point"). Using /grip
                    // here is what made an old build feel like the
                    // controller was tilted up.
                    //
                    // Strictly gated on a fresh left-controller aim
                    // pose this frame -- if the controller is not
                    // tracked (not yet woken up, line of sight
                    // blocked, etc.) the player just doesn't move. We
                    // never invent a direction.
                    if (aim_world_valid[0]) {
                        const Vec3 aim =
                            aim_world_rot[0] * Vec3(0.0f, 0.0f, -1.0f);
                        Vec3 candidate(aim.x, 0.0f, aim.z);
                        const float flen = glm::length(candidate);
                        if (flen > 1e-5f) {
                            fwd = candidate / flen;
                            right = glm::normalize(
                                glm::cross(fwd, Vec3(0.0f, 1.0f, 0.0f)));
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

                    // 1 Hz live-trace of the slide basis. Every line
                    // proves which tracked pose drove motion this
                    // second: turning the head 180 deg in head-relative
                    // mode flips the (fx,fz) direction within ~1s; a
                    // controller-relative slide keeps fx,fz aligned to
                    // the LEFT controller no matter where you look. If
                    // you ever see a fixed direction across multiple
                    // seconds while you physically rotate, something
                    // upstream is wrong -- file the log.
                    static float trace_accum = 0.0f;
                    trace_accum += dt;
                    if (trace_accum >= 1.0f) {
                        const char* mode_name =
                            (locomotion_ == LocomotionMode::SlideHeadRelative)
                                ? "head_rel"
                                : "ctrl_rel";
                        PTV_INFO(
                            "Slide[{}] fwd=({:+.2f},{:+.2f}) stick=({:+.2f},"
                            "{:+.2f}) speed={:.2f}m/s rig=({:+.2f},{:+.2f}) "
                            "head_fresh={}",
                            mode_name, fwd.x, fwd.z, lx, ly,
                            current_slide_speed_mps_, rig_.pos_x, rig_.pos_z,
                            head_pose_fresh ? "yes" : "no");
                        trace_accum = 0.0f;
                    }
                } else if (left_mag > 0.05f && !have_basis) {
                    // The user is actively pushing the stick but we
                    // refused to slide because we don't have a fresh
                    // tracked pose to use as the forward basis.
                    // Surface this once per second so the runtime log
                    // tells us *why* the player isn't moving (head
                    // pose unavailable / left controller not tracked /
                    // VIEW reference space invalid).
                    static float warn_accum = 0.0f;
                    warn_accum += dt;
                    if (warn_accum >= 1.0f) {
                        const char* mode_name =
                            (locomotion_ == LocomotionMode::SlideHeadRelative)
                                ? "head_rel"
                                : "ctrl_rel";
                        PTV_WARN(
                            "Slide[{}] no basis (head_fresh={}, "
                            "left_hand_valid={}, head_valid={}). "
                            "Player won't move until tracking is restored.",
                            mode_name, head_pose_fresh ? "yes" : "no",
                            hand_valid[0] ? "yes" : "no",
                            input.head_valid ? "yes" : "no");
                        warn_accum = 0.0f;
                    }
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

        // ------------------------------------------------------------
        // Build the read-only input snapshot handed to every mechanic.
        // All poses are already rig-composed (world space) so generated
        // mechanics can compare them directly against entity transforms.
        // Grab state (hands_[h].held) was resolved above, so weapon/tool
        // mechanics can see which hand holds them THIS frame.
        // ------------------------------------------------------------
        InputSnapshot snapshot;
        snapshot.dt = dt;
        for (int h = 0; h < 2; ++h) {
            auto& hs = snapshot.hands[h];
            hs.trigger = input.trigger[h];
            hs.grip = input.grip[h];
            hs.thumbstick = Vec2(input.thumbstick[h].x, input.thumbstick[h].y);
            hs.prev_trigger = prev_input_.trigger[h];
            hs.prev_grip = prev_input_.grip[h];
            if (hand_valid[h]) {
                hs.hand_position = hand_world_pos[h];
                hs.hand_rotation = hand_world_rot[h];
                hs.hand_valid = true;
            }
            if (aim_world_valid[h]) {
                hs.aim_position =
                    mat_translation(rig_mat * xr_pose_to_matrix(aim_poses[h]));
                hs.aim_rotation = aim_world_rot[h];
                hs.aim_valid = true;
            }
            hs.held_entity = (hands_[h].held == entt::null)
                                 ? InputSnapshot::kInvalidEntity
                                 : static_cast<std::uint32_t>(hands_[h].held);
        }
        snapshot.button_a = input.button_a;
        snapshot.button_b = input.button_b;
        snapshot.button_x = input.button_x;
        snapshot.button_y = input.button_y;
        snapshot.prev_button_a = prev_input_.button_a;
        snapshot.prev_button_b = prev_input_.button_b;
        snapshot.prev_button_x = prev_input_.button_x;
        snapshot.prev_button_y = prev_input_.button_y;
        if (input.head_valid) {
            snapshot.head_position = head_world_pos;
            snapshot.head_rotation = head_world_rot;
            snapshot.head_valid = true;
        }

        mechanics_.update(world_, dt, snapshot);

        // Remember this frame's raw input for next frame's edge detection.
        prev_input_ = input;

        // ------------------------------------------------------------
        // Render frame.
        //
        // ``begin_xr_frame()`` already opened the OpenXR frame above,
        // so ``render_frame()`` reuses the pending FrameState and
        // skips its own xrWaitFrame/xrBeginFrame. If
        // ``xr_should_render`` is false the runtime asked us to skip
        // rendering and ``begin_xr_frame()`` already submitted the
        // empty end-frame, so we just bypass the render block. If the
        // renderer is not yet ready (initial frames before Vulkan
        // swapchains were built) we have to manually submit an empty
        // end-frame to balance the wait/begin pair.
        // ------------------------------------------------------------
        if (xr_should_render && renderer_ready_ && xr_.is_running()) {
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
            // We wait for ``menu_.anchor_set`` (which only flips true once
            // a real HMD pose has been observed) before drawing so the
            // panel never appears at the stale default position for a
            // frame at session start -- it just pops directly into its
            // comfortable eye-relative anchor.
            if (locomotion_menu_enabled_ && menu_.page != MenuPage::Closed &&
                menu_.anchor_set) {
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

                // Layout (matches confirm logic above):
                //   Root page    : LEFT = Walking, RIGHT = Teleport
                //   Sub-menu page: LEFT = Head-Relative, RIGHT = Controller-Relative
                // Procedural fallbacks: 1=person, 0=portal, 2=headset, 3=joystick.
                int slot_left, slot_right;
                int proc_left, proc_right;
                if (menu_.page == MenuPage::Root) {
                    slot_left  = menu_icon_walking_slot_;
                    slot_right = menu_icon_teleport_slot_;
                    proc_left  = 1;  // green person
                    proc_right = 0;  // blue portal
                } else {
                    slot_left  = menu_icon_head_relative_slot_;
                    slot_right = menu_icon_controller_relative_slot_;
                    proc_left  = 2;  // headset
                    proc_right = 3;  // joystick
                }
                Vec3 wc_a, wc_b;
                push_menu_pad(items, menu_world, Vec3(-0.35f, 0.0f, 0.0f),
                              slot_left,  proc_left,
                              menu_.highlighted == 0, &wc_a);
                push_menu_pad(items, menu_world, Vec3( 0.35f, 0.0f, 0.0f),
                              slot_right, proc_right,
                              menu_.highlighted == 1, &wc_b);

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
                    // NOTE: Head pose cache (last_head_forward_xz_,
                    // last_head_world_pos_, last_head_pose_valid_) is
                    // populated up-front from the freshly-located VIEW
                    // space in sync_input/locomotion -- not here -- so
                    // locomotion logic always sees this frame's head
                    // pose, never the previous frame's render snapshot.
                    renderer_.draw_frame(targets, items, render_env_);
                });
        } else if (xr_should_render) {
            // begin_xr_frame() opened a real frame but renderer isn't
            // ready (e.g. Vulkan failed to initialize, or session is in
            // STOPPED). Submit an empty end-frame so we don't leak the
            // wait/begin pair.
            xr_.submit_pending_empty_frame();
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
    // Resolve + load the scene up front so the OpenXR application name and the
    // mirror-window title reflect *this project's* title -- that string is what
    // shows up in the Quest Link "recents" list. Without this every generated
    // game would advertise the generic host name (``ptv_starter_room``). The
    // heavier world / physics application still happens further below once
    // Vulkan and the physics world are initialized.
    scene_path_ = resolve_scene_path(cfg);
    bool scene_loaded = false;
    if (!scene_path_.empty()) {
        scene_ = LoadSceneJson(scene_path_);
        scene_loaded = true;
    }
    const std::string display_name =
        (scene_loaded && !scene_.title.empty()) ? scene_.title : cfg.app_name;

    try {
        window_ = std::make_unique<Window>(cfg.mirror_width, cfg.mirror_height,
                                            display_name + " [mirror]");
    } catch (const std::exception& e) {
        PTV_ERROR("Window init: {}", e.what());
        return false;
    }

    PTV_INFO("App: beginning OpenXR initialization (phase 1: instance/system)...");
    bool xr_ready = xr_.init_instance_and_system(display_name);

    VulkanContext::Config vcfg;
    vcfg.app_name = display_name;
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

    if (scene_loaded) {
        // ``scene_`` was already parsed above (for the window / XR title); now
        // that physics is live we apply it to the ECS world.
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

    // Cache the renderer environment (sky/sun/ambient/fog/point lights) from
    // the loaded scene so the renderer honors what the world-gen agent
    // authored. Safe to call whether or not a scene was loaded.
    build_render_env();

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

    // Pre-load locomotion-menu icons. The four GLBs live in the repo's
    // 3D_Objects/ folder; we walk up from the running .exe to find it so
    // both the starter-room sample and orchestrator-built games pick up
    // the same assets. ``kIconScale`` is the longest-axis size in metres
    // and is chosen to fit comfortably inside the 0.30 x 0.30 m pad.
    if (renderer_ready_) {
        const std::string root = find_3d_objects_dir();
        if (root.empty()) {
            PTV_WARN(
                "3D_Objects/ folder not found by walking up from the exe -- "
                "locomotion menu will use procedural fallback icons.");
        } else {
            namespace fs = std::filesystem;
            constexpr float kIconScale = 0.22f;
            auto load = [&](const char* name) -> int {
                fs::path p = fs::path(root) / name;
                std::error_code ec;
                if (!fs::exists(p, ec)) {
                    PTV_WARN("Locomotion menu icon missing: {}",
                             p.string());
                    return -1;
                }
                int slot = renderer_.load_glb(p.string(), kIconScale);
                if (slot < 0) {
                    PTV_WARN("Failed to load locomotion menu icon: {}",
                             p.string());
                }
                return slot;
            };
            menu_icon_walking_slot_  = load("Walking Locomotion.glb");
            menu_icon_teleport_slot_ = load("Teleport Locomotion.glb");
            menu_icon_head_relative_slot_ = load(
                "Head Relative (Sub Category of Walking Locomotion).glb");
            menu_icon_controller_relative_slot_ = load(
                "Controller Relative (Sub Category of Walking Locomotion).glb");
            PTV_INFO(
                "Locomotion menu icons loaded (walking={}, teleport={}, "
                "head-rel={}, controller-rel={}).",
                menu_icon_walking_slot_, menu_icon_teleport_slot_,
                menu_icon_head_relative_slot_,
                menu_icon_controller_relative_slot_);
        }
    }
    return true;
}

void Application::build_render_env() {
    Renderer::RenderEnv env{};

    // The world-generation agent OWNS the look. When it authored a sky block
    // we take its exact sky / sun / ambient / fog / exposure values -- the
    // engine imposes no brightness floor or overrides, so a scene can be as
    // bright or as dark as the agent intends. Defaults below only apply when a
    // scene authored NO sky (e.g. the hand-built starter room), so it still
    // looks good without regressing.
    if (scene_.sky.present) {
        env.has_sky = true;
        env.sky_top = scene_.sky.top;
        env.sky_horizon = scene_.sky.horizon;
        env.sun_direction = scene_.sky.sun_direction;  // travel dir (downward)
        env.sun_color = scene_.sky.sun_color;
        env.sun_intensity = scene_.sky.sun_intensity;
        env.ambient_ground = scene_.sky.ambient_color;
        env.ambient_intensity = scene_.sky.ambient_intensity;
        env.fog_density = scene_.sky.fog_density;
        env.exposure = scene_.sky.exposure;
    } else {
        // No authored sky: derive a pleasant daytime dome. The flat clear
        // color acts as the horizon so scenes that only set sky_color still
        // read sensibly, and the starter room gets a soft blue sky.
        env.has_sky = false;
        env.sky_top = Vec3(0.35f, 0.52f, 0.80f);
        env.sky_horizon = Vec3(0.72f, 0.80f, 0.90f);
        env.sun_direction = glm::normalize(Vec3(-0.35f, -0.80f, -0.40f));
        env.sun_color = Vec3(1.0f, 0.97f, 0.90f);
        env.sun_intensity = 1.0f;
        env.ambient_ground = Vec3(0.20f, 0.21f, 0.24f);
        env.ambient_intensity = scene_.ambient_intensity;  // top-level default
        env.fog_density = 0.012f;
        env.exposure = 1.0f;
        // First directional light (if any) overrides the default sun so
        // authored lighting still drives the look without a full sky block.
        for (const auto& l : scene_.lights) {
            if (l.kind == "directional") {
                if (glm::length(l.direction) > 1e-5f) {
                    env.sun_direction = glm::normalize(l.direction);
                }
                env.sun_color = l.color;
                env.sun_intensity = l.intensity;
                break;
            }
        }
    }

    // --- Point lights (up to 8) ---------------------------------------
    // Range is author-controlled; fall back to a sensible default only when a
    // light left it unset (<= 0).
    for (const auto& l : scene_.lights) {
        if (l.kind != "point") continue;
        if (env.points.size() >= 8) break;
        Renderer::PointLight p{};
        p.position = l.transform.position;
        p.color = l.color;
        p.intensity = l.intensity;
        p.range = l.range > 0.0f ? l.range : 20.0f;
        env.points.push_back(p);
    }

    render_env_ = std::move(env);
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
