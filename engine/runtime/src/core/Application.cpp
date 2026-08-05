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

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "ptv/core/Logger.h"
#include "ptv/ecs/TransformOffset.h"
#include "ptv/scene/SceneLoader.h"

// Desktop-only host pieces: the GLFW mirror window and the ImGui debug overlay.
// On Quest (standalone Android) there is no desktop window; the loop is driven
// by the NativeActivity event queue and DebugUI is not shipped.
#if !defined(__ANDROID__)
#include <GLFW/glfw3.h>

#include "ptv/debug/DebugUI.h"
#else
#include <android_native_app_glue.h>

#include "../platform/android/AndroidPlatform.h"
#endif

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
// procedural icons).
//
// Desktop-only: it depends on the Win32 ``GetModuleFileNameW`` to find the
// exe, and the ``3D_Objects/`` tree only exists in the desktop repo layout.
// On Quest the menu icons are packaged differently (and fall back to
// procedural icons), so this returns ``{}`` on Android.
static std::string find_3d_objects_dir() {
#if defined(__ANDROID__)
    return {};
#else
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
#endif
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

// ---------------------------------------------------------------------------
// Mechanic services
//
// These are the ONLY way a mechanic (built-in or generated) can move the
// player, touch physics, or draw. Every mutating entry point validates and
// clamps its arguments: generated code is untrusted, so a bad value must
// degrade into "nothing happens", never into a player flung out of the world
// or a NaN poisoning the physics solver.
// ---------------------------------------------------------------------------
namespace {

bool is_finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool is_finite(const Quat& q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) &&
           std::isfinite(q.z);
}

// Generous bounds: legitimate gameplay never approaches these, so hitting one
// means a mechanic is misbehaving.
constexpr float kMaxTranslatePerCall = 2.0f;    // metres
constexpr float kMaxImpulseMagnitude = 200.0f;  // kg*m/s

// Visual transform "juice" bounds. A recoil kick is centimetres and degrees;
// anything past these is a bug, and letting it through would visibly tear the
// mesh away from its physics body.
constexpr float kMaxFxOffsetMeters = 0.25f;
constexpr float kMaxFxRotationDegrees = 45.0f;
constexpr float kMinFxRecoverTau = 0.01f;
constexpr float kMaxFxRecoverTau = 1.0f;

// Haptics. A gameplay pulse is tens of milliseconds; half a second is already
// a long rumble. The cooldown stops a mechanic that pulses every frame from
// pinning the controller into a continuous buzz -- the pulses would overlap
// into one flat vibration that carries no information anyway.
constexpr float kMaxHapticDuration = 0.5f;
constexpr float kDefaultHapticFrequency = 160.0f;  // Hz, generic "thump"
constexpr float kHapticCooldownSeconds = 0.03f;

/** Log a clamp the first time each distinct site trips, so a broken mechanic
 *  is visible in logcat without spamming every frame. */
#define PTV_WARN_CLAMP_ONCE(...)          \
    do {                                  \
        static bool _warned = false;      \
        if (!_warned) {                   \
            _warned = true;               \
            PTV_WARN(__VA_ARGS__);        \
        }                                 \
    } while (0)

}  // namespace

class Application::LocomotionService final : public ILocomotion {
public:
    explicit LocomotionService(Application* app) : app_(app) {}

    Vec3 position() const override {
        return Vec3(app_->rig_.pos_x, app_->rig_.pos_y, app_->rig_.pos_z);
    }
    float yaw_radians() const override { return app_->rig_.yaw_rad; }

    void translate(const Vec3& delta) override {
        if (!is_finite(delta)) {
            PTV_WARN_CLAMP_ONCE("locomotion: rejected non-finite translate");
            return;
        }
        Vec3 d = delta;
        const float len = glm::length(d);
        if (len > kMaxTranslatePerCall) {
            PTV_WARN_CLAMP_ONCE(
                "locomotion: translate of {:.1f} m clamped to {:.1f} m", len,
                kMaxTranslatePerCall);
            d *= (kMaxTranslatePerCall / len);
        }
        app_->rig_.pos_x += d.x;
        app_->rig_.pos_y += d.y;
        app_->rig_.pos_z += d.z;
    }

    void rotate_yaw(float radians) override {
        if (!std::isfinite(radians)) {
            PTV_WARN_CLAMP_ONCE("locomotion: rejected non-finite rotate_yaw");
            return;
        }
        // A single call should never spin more than half a turn.
        constexpr float kPi = 3.14159265358979323846f;
        app_->rig_.yaw_rad += std::max(-kPi, std::min(kPi, radians));
    }

    void teleport_to(const Vec3& target) override {
        if (!is_finite(target)) {
            PTV_WARN_CLAMP_ONCE("locomotion: rejected non-finite teleport_to");
            return;
        }
        Vec3 dst = target;
        // Cap horizontal distance to what the scene allows.
        const Vec3 here(app_->rig_.pos_x, app_->rig_.pos_y, app_->rig_.pos_z);
        Vec3 flat(dst.x - here.x, 0.0f, dst.z - here.z);
        const float dist = glm::length(flat);
        const float max_d = std::max(0.1f, app_->teleport_max_horizontal_m_);
        if (dist > max_d) {
            PTV_WARN_CLAMP_ONCE(
                "locomotion: teleport of {:.1f} m clamped to {:.1f} m", dist,
                max_d);
            flat *= (max_d / dist);
            dst.x = here.x + flat.x;
            dst.z = here.z + flat.z;
        }
        // Never drop the player under the floor plane.
        if (dst.y < app_->teleport_floor_y_) {
            dst.y = app_->teleport_floor_y_;
        }
        app_->rig_.pos_x = dst.x;
        app_->rig_.pos_y = dst.y;
        app_->rig_.pos_z = dst.z;
    }

    // NOTE: must be fully qualified. Inside a nested class of Application,
    // unqualified ``LocomotionMode`` resolves to Application's own private
    // enum, not the public API one.
    ::ptv::LocomotionMode active_mode() const override {
        switch (app_->locomotion_) {
            case Application::LocomotionMode::SlideHeadRelative:
                return ::ptv::LocomotionMode::SlideHeadRelative;
            case Application::LocomotionMode::SlideControllerRelative:
                return ::ptv::LocomotionMode::SlideControllerRelative;
            case Application::LocomotionMode::Teleport:
            default:
                return ::ptv::LocomotionMode::Teleport;
        }
    }
    void set_active_mode(::ptv::LocomotionMode mode) override {
        switch (mode) {
            case ::ptv::LocomotionMode::SlideHeadRelative:
                app_->locomotion_ = Application::LocomotionMode::SlideHeadRelative;
                break;
            case ::ptv::LocomotionMode::SlideControllerRelative:
                app_->locomotion_ =
                    Application::LocomotionMode::SlideControllerRelative;
                break;
            case ::ptv::LocomotionMode::Teleport:
            default:
                app_->locomotion_ = Application::LocomotionMode::Teleport;
                break;
        }
    }

    bool input_locked() const override {
        // The locomotion menu owns the sticks while it is open.
        return app_->locomotion_menu_enabled_ &&
               app_->menu_.page != Application::MenuPage::Closed;
    }

    const LocomotionTunables& tunables() const override {
        // Refreshed from the scene's player block each call so a mechanic
        // always sees the values the scene actually authored.
        tunables_.slide_max_speed_mps = app_->slide_max_speed_mps_;
        tunables_.slide_accel_tau_s = app_->slide_accel_tau_s_;
        tunables_.slide_decel_tau_s = app_->slide_decel_tau_s_;
        tunables_.teleport_launch_speed_mps = app_->teleport_launch_speed_mps_;
        tunables_.teleport_max_horizontal_m = app_->teleport_max_horizontal_m_;
        tunables_.teleport_floor_y = app_->teleport_floor_y_;
        tunables_.snap_turn_degrees = app_->snap_turn_degrees_;
        tunables_.snap_turn_cooldown_s = app_->snap_turn_cooldown_s_;
        return tunables_;
    }

private:
    Application* app_;
    mutable LocomotionTunables tunables_{};
};

class Application::PhysicsAccessService final : public IPhysicsAccess {
public:
    explicit PhysicsAccessService(Application* app) : app_(app) {}

    bool raycast(const Vec3& origin, const Vec3& dir, float max_dist,
                 PhysicsRayHit& out) const override {
        out = PhysicsRayHit{};
        if (!is_finite(origin) || !is_finite(dir) || !std::isfinite(max_dist)) {
            return false;
        }
        Vec3 hit(0.0f);
        Vec3 normal(0.0f, 1.0f, 0.0f);
        if (!app_->physics_.raycast(origin, dir, max_dist, hit, normal)) {
            return false;
        }
        out.point = hit;
        out.normal = normal;
        out.hit = true;
        return true;
    }

    bool has_body(std::uint32_t entity) const override {
        return app_->physics_.has_body(entity);
    }

    void set_kinematic(std::uint32_t entity, bool kinematic) override {
        app_->physics_.set_kinematic(entity, kinematic);
    }

    void move_kinematic(std::uint32_t entity, const Vec3& pos, const Quat& rot,
                        float dt) override {
        if (!is_finite(pos) || !is_finite(rot) || !std::isfinite(dt)) {
            PTV_WARN_CLAMP_ONCE("physics: rejected non-finite move_kinematic");
            return;
        }
        app_->physics_.move_kinematic(entity, pos, rot, dt);
    }

    void apply_impulse(std::uint32_t entity, const Vec3& impulse) override {
        if (!is_finite(impulse)) {
            PTV_WARN_CLAMP_ONCE("physics: rejected non-finite impulse");
            return;
        }
        Vec3 imp = impulse;
        const float mag = glm::length(imp);
        if (mag > kMaxImpulseMagnitude) {
            PTV_WARN_CLAMP_ONCE(
                "physics: impulse of {:.0f} clamped to {:.0f}", mag,
                kMaxImpulseMagnitude);
            imp *= (kMaxImpulseMagnitude / mag);
        }
        app_->physics_.apply_impulse(entity, imp);
    }

private:
    Application* app_;
};

class Application::DebugDrawService final : public IDebugDraw {
public:
    explicit DebugDrawService(Application* app) : app_(app) {}

    void line(const Vec3& a, const Vec3& b, const Vec4& color,
              float thickness) override {
        if (!is_finite(a) || !is_finite(b)) return;
        push_segment(a, b, color, thickness);
    }

    void polyline(const Vec3* points, std::size_t count, const Vec4& color,
                  float thickness) override {
        if (points == nullptr || count < 2) return;
        for (std::size_t i = 1; i < count; ++i) {
            if (!is_finite(points[i - 1]) || !is_finite(points[i])) continue;
            push_segment(points[i - 1], points[i], color, thickness);
        }
    }

    void marker(const Vec3& center, float radius, const Vec4& color) override {
        if (!is_finite(center) || !std::isfinite(radius)) return;
        const float r = std::max(0.01f, radius);
        Renderer::DrawItem it{};
        // Unit cube spans -1..1, so scale is a HALF extent: a flat disc of
        // diameter 2*r, 2cm thick.
        it.model = glm::translate(Mat4(1.0f), center) *
                   glm::scale(Mat4(1.0f), Vec3(r, 0.01f, r));
        it.color = color;
        it.material_id = 3.0f;
        it.mesh_slot = -1;
        app_->mech_draw_items_.push_back(it);
    }

private:
    // A segment is a thin scaled cube spanning a..b -- the same trick the
    // engine already used for the teleport arc, but oriented in full 3D
    // rather than yaw-only, so a steep arc's segments actually follow it.
    void push_segment(const Vec3& a, const Vec3& b, const Vec4& color,
                      float thickness) {
        const Vec3 delta = b - a;
        const float len = glm::length(delta);
        if (len < 1e-5f) return;
        const Vec3 dir = delta / len;
        const Vec3 up(0.0f, 1.0f, 0.0f);

        // Rotation taking the cube's local +Y onto ``dir``.
        Quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        const float cos_a = std::max(-1.0f, std::min(1.0f, glm::dot(up, dir)));
        Vec3 axis = glm::cross(up, dir);
        const float axis_len = glm::length(axis);
        if (axis_len > 1e-5f) {
            rot = glm::angleAxis(std::acos(cos_a), axis / axis_len);
        } else if (cos_a < 0.0f) {
            rot = glm::angleAxis(3.14159265f, Vec3(1.0f, 0.0f, 0.0f));
        }

        const float half_t = std::max(0.001f, thickness) * 0.5f;
        Renderer::DrawItem it{};
        it.model = glm::translate(Mat4(1.0f), (a + b) * 0.5f) *
                   glm::mat4_cast(rot) *
                   glm::scale(Mat4(1.0f), Vec3(half_t, len * 0.5f, half_t));
        it.color = color;
        it.material_id = 3.0f;
        it.mesh_slot = -1;
        app_->mech_draw_items_.push_back(it);
    }

    Application* app_;
};

void Application::ensure_player_locomotion_bindings() {
    // The runtime locomotion menu lets the player pick teleport, head-relative
    // slide or controller-relative slide at ANY time. Now that each of those
    // is a real mechanic rather than a hard-coded branch, a mode with nothing
    // bound would silently do nothing -- the player would flip to sliding and
    // simply be unable to move.
    //
    // Most scenes bind only what their prompt asked for (the starter room
    // binds teleport and snap turn), so the engine guarantees the rest here.
    // Anything the scene DID bind is left completely alone, params included.
    static constexpr const char* kRequired[] = {
        "builtin.locomotion.teleport",
        "builtin.locomotion.slide.head_relative",
        "builtin.locomotion.slide.controller_relative",
        "builtin.locomotion.snap_turn",
    };
    // Aliases the hand-authored sample uses for the same mechanics.
    auto satisfies = [](const std::string& bound, const char* required) {
        if (bound == required) return true;
        if (bound == "builtin.teleport" &&
            std::string(required) == "builtin.locomotion.teleport") {
            return true;
        }
        if (bound == "builtin.snap_turn" &&
            std::string(required) == "builtin.locomotion.snap_turn") {
            return true;
        }
        return false;
    };

    std::vector<std::string> bound_ids;
    for (auto e : world_.registry().view<CMechanicBinding>()) {
        for (const auto& entry :
             world_.registry().get<CMechanicBinding>(e).entries) {
            bound_ids.push_back(entry.builtin_id.empty() ? entry.id
                                                         : entry.builtin_id);
        }
    }

    std::vector<std::string> missing;
    for (const char* required : kRequired) {
        bool found = false;
        for (const auto& bound : bound_ids) {
            if (satisfies(bound, required)) {
                found = true;
                break;
            }
        }
        if (!found) missing.emplace_back(required);
    }
    if (missing.empty()) return;

    const auto holder = world_.registry().create();
    world_.registry().emplace<CTag>(holder, CTag{{"player_locomotion"}});
    CMechanicBinding binding;
    for (const auto& id : missing) {
        binding.entries.push_back(CMechanicBinding::Entry{id, id, "{}"});
    }
    world_.registry().emplace<CMechanicBinding>(holder, std::move(binding));
    PTV_INFO("Auto-bound {} locomotion mechanic(s) the scene did not declare",
             missing.size());
}

class Application::HandsService final : public IHands {
public:
    explicit HandsService(Application* app) : app_(app) {}

    std::uint32_t held_entity(int hand) const override {
        if (hand < 0 || hand > 1) return InputSnapshot::kInvalidEntity;
        const auto held = app_->hands_[hand].held;
        return held == entt::null ? InputSnapshot::kInvalidEntity
                                  : static_cast<std::uint32_t>(held);
    }

    void set_held_entity(int hand, std::uint32_t entity) override {
        if (hand < 0 || hand > 1) return;
        app_->hands_[hand].held = (entity == InputSnapshot::kInvalidEntity)
                                      ? entt::null
                                      : static_cast<entt::entity>(entity);
    }

private:
    Application* app_;
};

class Application::TransformFxService final : public ITransformFx {
public:
    explicit TransformFxService(Application* app) : app_(app) {}

    void impulse(std::uint32_t entity, const TransformImpulse& kick) override {
        World& w = app_->world_;
        if (!w.valid(entity)) return;
        if (!is_finite(kick.local_offset) || !is_finite(kick.local_euler_deg) ||
            !std::isfinite(kick.recover_tau_s)) {
            PTV_WARN_CLAMP_ONCE("fx: rejected non-finite transform impulse");
            return;
        }

        Vec3 offset = kick.local_offset;
        Vec3 euler = kick.local_euler_deg;
        const float offset_mag = glm::length(offset);
        if (offset_mag > kMaxFxOffsetMeters) {
            PTV_WARN_CLAMP_ONCE("fx: offset of {:.2f}m clamped to {:.2f}m",
                                offset_mag, kMaxFxOffsetMeters);
            offset *= (kMaxFxOffsetMeters / offset_mag);
        }
        const float euler_mag = glm::length(euler);
        if (euler_mag > kMaxFxRotationDegrees) {
            PTV_WARN_CLAMP_ONCE("fx: rotation of {:.1f}deg clamped to {:.1f}deg",
                                euler_mag, kMaxFxRotationDegrees);
            euler *= (kMaxFxRotationDegrees / euler_mag);
        }
        const float tau = std::clamp(kick.recover_tau_s, kMinFxRecoverTau,
                                     kMaxFxRecoverTau);

        auto& off = w.has<CTransformOffset>(entity)
                        ? w.get<CTransformOffset>(entity)
                        : w.emplace_or_replace<CTransformOffset>(entity);

        // Accumulate rather than overwrite so rapid fire compounds instead of
        // restarting the same kick, then re-clamp the total.
        off.position += offset;
        if (glm::length(off.position) > kMaxFxOffsetMeters) {
            off.position = glm::normalize(off.position) * kMaxFxOffsetMeters;
        }
        const Quat kick_rot = glm::quat(glm::radians(euler));
        off.rotation = glm::normalize(kick_rot * off.rotation);
        off.recover_tau_s = tau;
        off.active = true;
    }

    void clear(std::uint32_t entity) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CTransformOffset>(entity)) return;
        auto& off = w.get<CTransformOffset>(entity);
        off.position = off.target_position;
        off.rotation = off.target_rotation;
        off.active = false;
    }

private:
    Application* app_;
};

class Application::HapticsService final : public IHaptics {
public:
    explicit HapticsService(Application* app) : app_(app) {}

    void pulse(int hand, float duration_s, float amplitude) override {
        pulse_at(hand, duration_s, kDefaultHapticFrequency, amplitude);
    }

    void pulse_at(int hand, float duration_s, float frequency_hz,
                  float amplitude) override {
        if (hand < 0 || hand > 1) return;
        if (!std::isfinite(duration_s) || !std::isfinite(frequency_hz) ||
            !std::isfinite(amplitude)) {
            PTV_WARN_CLAMP_ONCE("haptics: rejected non-finite pulse");
            return;
        }
        if (amplitude <= 0.0f || duration_s <= 0.0f) return;

        // Rate limit per hand against the frame clock. A mechanic that pulses
        // unconditionally in on_update would otherwise restart the effect
        // every frame and produce a flat, meaningless buzz.
        const double now = app_->haptic_clock_s_;
        if (now - last_pulse_s_[hand] < kHapticCooldownSeconds) return;
        last_pulse_s_[hand] = now;

        app_->xr_.apply_haptic(hand, std::min(duration_s, kMaxHapticDuration),
                               frequency_hz, std::clamp(amplitude, 0.0f, 1.0f));
    }

private:
    Application* app_;
    double last_pulse_s_[2]{-1.0, -1.0};
};

class Application::SocketsService final : public ISockets {
public:
    explicit SocketsService(Application* app) : app_(app) {}

    bool has_socket(std::uint32_t entity, const char* name) const override {
        if (name == nullptr) return false;
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CMesh>(entity)) return false;
        const auto& sockets = w.get<CMesh>(entity).sockets;
        return sockets.find(name) != sockets.end();
    }

    Mat4 socket_world(std::uint32_t entity, const char* name) const override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CTransform>(entity)) return Mat4(1.0f);
        const Mat4 base = w.get<CTransform>(entity).t.matrix();

        if (name != nullptr && w.has<CMesh>(entity)) {
            const auto& mesh = w.get<CMesh>(entity);
            const auto it = mesh.sockets.find(name);
            if (it != mesh.sockets.end()) {
                return base * it->second.matrix();
            }
        }
        return base * fallback_local(w, entity);
    }

private:
    /** A pose on the forward face of the entity's bounds.
     *
     *  Chosen over identity deliberately: a projectile from the front of the
     *  object reads as slightly misaligned, while one from the object's
     *  centre spawns inside the player's palm. Degrade visibly-wrong, not
     *  catastrophically-wrong. */
    static Mat4 fallback_local(const World& w, std::uint32_t entity) {
        float radius = 0.0f;
        if (w.has<CMesh>(entity)) {
            const auto& mesh = w.get<CMesh>(entity);
            radius = mesh.bounds_radius_m > 0.0f
                         ? mesh.bounds_radius_m
                         : std::max(0.05f, mesh.target_scale_meters * 0.5f);
        }
        // -Z is forward in the glTF / OpenXR convention the engine uses.
        return glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, -radius));
    }

    Application* app_;
};

Application::Application() = default;
Application::~Application() = default;

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
        // Monotonic seconds-since-start, used by the haptics rate limiter.
        haptic_clock_s_ += dt;

#if !defined(__ANDROID__)
        window_->poll_events();
        if (window_->should_close()) running_ = false;
#else
        // Pump the NativeActivity event queue (non-blocking). APP_CMD_*
        // lifecycle events are handled by the onAppCmd callback in
        // AndroidMain.cpp; when the activity is torn down
        // android_app::destroyRequested is set.
        //
        // ALooper_pollAll is removed as of NDK r27 (racy vs ALooper_wake), so
        // we call ALooper_pollOnce in a loop and treat every return value as a
        // reason to process/continue. A zero timeout keeps the frame loop
        // running; the OpenXR session state machine (XrRuntime::poll_events)
        // gates actual rendering until the runtime is READY/focused.
        if (android_app* app = current_android_app()) {
            int events = 0;
            android_poll_source* source = nullptr;
            while (ALooper_pollOnce(0, nullptr, &events,
                                    reinterpret_cast<void**>(&source)) >= 0) {
                if (source) source->process(app, source);
                if (app->destroyRequested) { running_ = false; break; }
            }
        }
#endif

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
        // Start the work clock AFTER xrWaitFrame has unblocked. The adaptive
        // governor must react to how long OUR frame takes, not to the interval
        // between frames: the runtime owns the pacing and legitimately slows
        // it down (headset idle/doffed, power saving), which would otherwise
        // look identical to us missing the frame budget.
        const auto frame_work_start = clock::now();

        XrRuntime::InputState input;
        xr_.sync_input(input);

        // Right-stick snap turn now lives in the built-in mechanic
        // ``builtin.locomotion.snap_turn`` (Input phase), not here.

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

        // Thumbstick dead zone + cardinal snap now live in the shared
        // mechanics/builtin/StickFilter.h, applied by whichever locomotion
        // mechanic is reading the stick.

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
        // Snapshot builder.
        //
        // Called TWICE per frame: once to give Input-phase mechanics a view
        // of the world before they run, and again afterwards to freeze the
        // immutable snapshot everything else sees. The second build is what
        // removes the one-frame lag on ``held_entity``: grab has already
        // resolved by then, so a weapon mechanic notices it was picked up on
        // the SAME frame rather than the next one.
        // ------------------------------------------------------------
        auto build_snapshot = [&]() {
            InputSnapshot s;
            s.dt = dt;
            for (int h = 0; h < 2; ++h) {
                auto& hs = s.hands[h];
                hs.trigger = input.trigger[h];
                hs.grip = input.grip[h];
                hs.thumbstick =
                    Vec2(input.thumbstick[h].x, input.thumbstick[h].y);
                hs.prev_trigger = prev_input_.trigger[h];
                hs.prev_grip = prev_input_.grip[h];
                hs.trigger_touched = input.trigger_touch[h];
                hs.thumbrest_touched = input.thumbrest_touch[h];
                hs.prev_trigger_touched = prev_input_.trigger_touch[h];
                hs.prev_thumbrest_touched = prev_input_.thumbrest_touch[h];
                if (hand_valid[h]) {
                    hs.hand_position = hand_world_pos[h];
                    hs.hand_rotation = hand_world_rot[h];
                    hs.hand_valid = true;
                }
                if (aim_world_valid[h]) {
                    hs.aim_position = mat_translation(
                        rig_mat * xr_pose_to_matrix(aim_poses[h]));
                    hs.aim_rotation = aim_world_rot[h];
                    hs.aim_valid = true;
                }
                hs.velocity = hands_[h].world_velocity;
                hs.held_entity =
                    (hands_[h].held == entt::null)
                        ? InputSnapshot::kInvalidEntity
                        : static_cast<std::uint32_t>(hands_[h].held);
            }
            s.button_a = input.button_a;
            s.button_b = input.button_b;
            s.button_x = input.button_x;
            s.button_y = input.button_y;
            s.prev_button_a = prev_input_.button_a;
            s.prev_button_b = prev_input_.button_b;
            s.prev_button_x = prev_input_.button_x;
            s.prev_button_y = prev_input_.button_y;
            if (input.head_valid) {
                s.head_position = head_world_pos;
                s.head_rotation = head_world_rot;
                s.head_valid = true;
            }
            return s;
        };

        // ------------------------------------------------------------
        // Smoothed hand velocity.
        //
        // Part of the input layer, not gameplay: it is a derived reading of
        // the tracked pose, published on the snapshot so throw (and any
        // generated mechanic that cares about a swing) can use it. Raw
        // per-frame differences of Quest 3 inside-out tracking are far too
        // noisy to throw with, hence the exponential average.
        // ------------------------------------------------------------
        for (int h = 0; h < 2; ++h) {
            auto& hs = hands_[h];
            if (!hand_valid[h]) {
                hs.had_prev_pos = false;
                hs.world_velocity = Vec3(0.0f);
                continue;
            }
            if (hs.had_prev_pos && dt > 0.0f) {
                const Vec3 inst = (hand_world_pos[h] - hs.prev_world_pos) / dt;
                // alpha = 0.4 balances responsiveness against camera jitter.
                hs.world_velocity = hs.world_velocity * 0.6f + inst * 0.4f;
            }
            hs.prev_world_pos = hand_world_pos[h];
            hs.had_prev_pos = true;
        }

        // ------------------------------------------------------------
        // Input-phase mechanics.
        //
        // These run BEFORE the snapshot is frozen and BEFORE the physics
        // step, so anything they decide (where the player is, what each hand
        // holds) is visible to every Simulate-phase mechanic this same frame.
        // Locomotion, grab and throw all live here.
        // ------------------------------------------------------------
        mech_draw_items_.clear();
        {
            const InputSnapshot provisional = build_snapshot();
            mechanics_.update(world_, dt, provisional, mech_ctx_,
                              MechanicPhase::Input);
        }

        // Left-stick SLIDE locomotion now lives in the built-in mechanics
        // builtin.locomotion.slide.head_relative and
        // builtin.locomotion.slide.controller_relative (Input phase). They
        // gate themselves on ILocomotion::active_mode() and input_locked(),
        // so the locomotion menu still suppresses movement while it is open.

        // Grab, throw and the kinematic follow for held objects now live in
        // the builtin.grab / builtin.throw mechanics, which ran in the Input
        // phase above. All this frame still owns is the hand velocity that
        // feeds them, computed further up with the rest of the input layer.

        // ------------------------------------------------------------
        // Freeze the immutable snapshot every other mechanic sees.
        //
        // Built HERE -- after grab/throw have resolved, before physics --
        // so ``held_entity`` is this frame's value, not last frame's.
        // ------------------------------------------------------------
        const InputSnapshot snapshot = build_snapshot();

        // ------------------------------------------------------------
        // Physics, then the ordinary (Simulate-phase) mechanics, which see
        // settled transforms and an up-to-date snapshot.
        // ------------------------------------------------------------
        physics_.step(dt);
        physics_.sync_to_world(world_);

        mechanics_.update(world_, dt, snapshot, mech_ctx_,
                          MechanicPhase::Simulate);

        // Decay the purely visual transform offsets (recoil kick, flinch)
        // AFTER Simulate so an impulse requested this frame is drawn this
        // frame, and before the draw list is built below.
        tick_transform_offsets(world_, dt);

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
            // Upload a bounded slice of finished background mesh loads before
            // building the draw list. Everything expensive (asset read + glTF
            // parse) already happened off-thread; this only does a couple of
            // GPU uploads so the Android event pump keeps running.
            renderer_.pump_streaming();

            // One conservative frustum covering BOTH eyes, built from the HMD
            // centre pose with a deliberately padded FOV (Quest 3 is ~110 deg
            // horizontal; 140 deg here comfortably contains both eye frusta
            // plus the IPD offset). Culling once is cheaper than twice and,
            // being conservative, can only ever draw a few extra objects at
            // the very edge -- never wrongly remove one the player can see.
            Renderer::ViewInfo view_info{};
            if (input.head_valid) {
                constexpr float kCullFovRadians = 2.443f;  // 140 degrees
                const Mat4 world_from_head =
                    glm::translate(Mat4(1.0f), head_world_pos) *
                    glm::mat4_cast(head_world_rot);
                const Mat4 cull_proj = glm::perspective(
                    kCullFovRadians, 1.0f, 0.05f, 2000.0f);
                view_info.camera_pos = head_world_pos;
                view_info.view_proj = cull_proj * glm::inverse(world_from_head);
                view_info.valid = true;
            }
            renderer_.build_items_from_world(world_, items, view_info);

            // Hand visuals: procedural palm + wrist + thumb in blue (left)
            // and red (right). The grab highlights light up brighter when
            // a grabbable is in range so the user knows where to squeeze.
            for (int h = 0; h < 2; ++h) {
                if (!hand_valid[h]) continue;
                // Mirrors builtin.grab's reach so the highlight lights up
                // exactly when a squeeze would actually pick something up.
                constexpr float kHoverRadius = 0.18f;
                const bool hovering = hands_[h].held != entt::null ||
                    find_grabbable(world_, hand_world_pos[h],
                                   kHoverRadius) != entt::null;
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

            // The teleport aim arc + landing marker used to be drawn here.
            // The teleport mechanic now submits them through IDebugDraw and
            // they arrive with the mechanic draw items below.

            // -------- Geometry submitted by mechanics via IDebugDraw -------
            // One-frame lifetime: mechanics re-submit every frame they want
            // something visible. Cleared at the top of the next frame.
            items.insert(items.end(), mech_draw_items_.begin(),
                         mech_draw_items_.end());

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

#if !defined(__ANDROID__)
        DebugUI::new_frame();
        XrFrameInfo fi{dt * 1000.0f, window_->width(), window_->height()};
        DebugUI::draw(world_, fi);
        DebugUI::render_to_mirror_window();
#endif

        // ------------------------------------------------------------
        // Adaptive quality + telemetry.
        //
        // Gated on xr_should_render, NOT merely on the session running: when
        // the headset is off the user's head the runtime tells us to skip
        // rendering, and the loop free-runs at ~30 ms. Feeding that to the
        // governor would look like a catastrophic frame-rate miss and drive
        // quality to the floor, so the player would don the headset to a
        // maximally degraded scene that then takes ~45 s to recover.
        // ------------------------------------------------------------
        if (xr_should_render && renderer_ready_ && xr_.is_running()) {
            // Our CPU+GPU cost for this frame (draw_frame waits on its fence,
            // so GPU time is included). This is the governor's input.
            const float work_s =
                std::chrono::duration<float>(clock::now() - frame_work_start)
                    .count();

            if (governor_.update(work_s)) {
                const auto& q = governor_.state();
                renderer_.set_lod_bias(q.lod_bias);
                renderer_.set_cull_distance(q.cull_distance_m);
                xr_.set_foveation_level(q.foveation_level);
            }

            telemetry_accum_s_ += dt;
            telemetry_work_accum_s_ += work_s;
            ++telemetry_frames_;
            telemetry_worst_frame_ms_ =
                std::max(telemetry_worst_frame_ms_, work_s * 1000.0f);
            if (telemetry_accum_s_ >= 1.0f && telemetry_frames_ > 0) {
                const auto& st = renderer_.frame_stats();
                const float frames = static_cast<float>(telemetry_frames_);
                PTV_INFO(
                    "perf: {:.1f} fps | work {:.1f} ms avg, {:.1f} ms worst | "
                    "draw {} of {} (cull frustum {} dist {}) | tris {} | "
                    "meshes {} resident ({} tris), {} streaming | "
                    "lod_bias {:.1f} cull {:.0f}m ffr {}",
                    frames / telemetry_accum_s_,
                    (telemetry_work_accum_s_ * 1000.0f) / frames,
                    telemetry_worst_frame_ms_, st.drawn,
                    st.entities_considered, st.culled_frustum,
                    st.culled_distance, st.visible_triangles,
                    st.resident_meshes, st.resident_triangles,
                    st.streaming_pending, renderer_.lod_bias(),
                    renderer_.cull_distance(), xr_.foveation_level());
                telemetry_accum_s_ = 0.0f;
                telemetry_work_accum_s_ = 0.0f;
                telemetry_frames_ = 0;
                telemetry_worst_frame_ms_ = 0.0f;
            }
        }

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

#if !defined(__ANDROID__)
    try {
        window_ = std::make_unique<Window>(cfg.mirror_width, cfg.mirror_height,
                                            display_name + " [mirror]");
    } catch (const std::exception& e) {
        PTV_ERROR("Window init: {}", e.what());
        return false;
    }
#endif  // desktop mirror window; Quest has no desktop surface

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

    // Stand up the services mechanics are allowed to call, then attach.
    // Must precede attach_bindings: on_attach receives the context.
    locomotion_svc_ = std::make_unique<LocomotionService>(this);
    physics_svc_ = std::make_unique<PhysicsAccessService>(this);
    draw_svc_ = std::make_unique<DebugDrawService>(this);
    hands_svc_ = std::make_unique<HandsService>(this);
    fx_svc_ = std::make_unique<TransformFxService>(this);
    haptics_svc_ = std::make_unique<HapticsService>(this);
    sockets_svc_ = std::make_unique<SocketsService>(this);
    mech_ctx_.locomotion = locomotion_svc_.get();
    mech_ctx_.physics = physics_svc_.get();
    mech_ctx_.draw = draw_svc_.get();
    mech_ctx_.hands = hands_svc_.get();
    mech_ctx_.fx = fx_svc_.get();
    mech_ctx_.haptics = haptics_svc_.get();
    mech_ctx_.sockets = sockets_svc_.get();

    ensure_player_locomotion_bindings();
    mechanics_.attach_bindings(world_, mech_ctx_);
    // NOTE: DebugUI is intentionally NOT initialized here. See
    // README / troubleshooting: ImGui >= 1.92 asserts on first Begin()
    // without a renderer backend that sets RendererHasTextures.

    if (xr_.is_running() || xr_.xr_session() != XR_NULL_HANDLE) {
        // Install the scene's device budget BEFORE initialize so the renderer
        // can pick MSAA from it instead of always taking the maximum the GPU
        // advertises (4x MSAA is a large, and often unnecessary, mobile cost).
        renderer_.set_performance(scene_.performance);
        governor_.configure(scene_.performance);
        renderer_ready_ = renderer_.initialize(vk_, xr_.color_format(),
                                               xr_.eye_width(),
                                               xr_.eye_height());
        // Baseline foveation for this scene's tier. No-op on runtimes without
        // the XR_FB_foveation extensions (e.g. PC VR over Quest Link).
        xr_.set_foveation_level(scene_.performance.foveation_level);
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
#if !defined(__ANDROID__)
    DebugUI::shutdown();
#endif
    renderer_.shutdown();
    renderer_ready_ = false;
    xr_.shutdown();
    physics_.shutdown();
    vk_.shutdown();
#if !defined(__ANDROID__)
    window_.reset();
#endif
}

}  // namespace ptv
