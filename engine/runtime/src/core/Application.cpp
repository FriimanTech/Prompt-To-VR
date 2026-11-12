#include "ptv/core/Application.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <deque>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// The player: rig, body and motor.
//
// Terminology matches Meta's "Locomotion Types" design guide:
//   * Teleport -- discrete: aim arc + commit.
//   * Sliding (Meta's term for thumbstick continuous motion), head- or
//     controller-relative.
// References:
//   https://developers.meta.com/horizon/design/locomotion-types/
//   https://developers.meta.com/horizon/design/locomotion-input-maps/
//
// Nothing about movement is implied here. Every locomotion, turning and
// settings-menu behaviour is a mechanic the scene binds explicitly; a scene
// that binds none is a stationary game (a rhythm game, a turret) and that is
// complete and valid. The engine used to inject teleport, both slides and
// snap turn into every scene and open a hard-coded locomotion menu at start;
// a game that also generated its own copies ran both, which is where the
// double teleport arc and the double snap came from.
//
// What this file does own is the player MOTOR (step_player_motor): mechanics
// only request movement, and the motor resolves it against the world through
// a Jolt CharacterVirtual. Stick movement is swept through the body, so walls
// stop it from any direction; physical walking is followed by the body and
// the head is pushed back out of walls; and the rig's floor tracks the
// ground under the body, so sliding over terrain follows it instead of
// accumulating height.
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

#include "core/GameBoards.h"
#include "core/NpcRuntime.h"
#include "core/PlayerParts.h"
#include "gameplay/EffectsSystem.h"
#include "gameplay/ProjectileSystem.h"
#include "gameplay/WeaponBoard.h"
#include "TextureGallery.h"
#include "InteractionVerifier.h"
#include "WeaponVerifier.h"
#include "mechanics/builtin/HandRig.h"
#include "ptv/api/BodySlots.h"
#include "ptv/api/Facing.h"
#include "ptv/core/Logger.h"
#include "ptv/ecs/Clone.h"
#include "ptv/ecs/Skeleton.h"
#include "ptv/ecs/TransformOffset.h"
#include "ptv/platform/AssetSource.h"
#include "ptv/save/SaveSystem.h"
#include "ptv/scene/SceneLoader.h"

// Where the generated hand meshes live. CMake supplies an absolute build path
// on desktop and the bare APK-relative "engine" on Quest; this fallback only
// matters for a hand-rolled build that sets neither.
#ifndef PTV_ENGINE_ASSET_DIR
#define PTV_ENGINE_ASSET_DIR "assets"
#endif

// Desktop-only host pieces: the GLFW mirror window and the ImGui debug overlay.
// On Quest (standalone Android) there is no desktop window; the loop is driven
// by the NativeActivity event queue and DebugUI is not shipped.
#if !defined(__ANDROID__)
#include <GLFW/glfw3.h>

#include "ptv/debug/DebugUI.h"
#else
#include <android_native_app_glue.h>
#include <sys/system_properties.h>

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

// The walking body when the scene does not say (``player.body_radius_m``,
// measured from the avatar's torso): a 0.3 m radius stops the player well
// short of anything they reach for.
constexpr float kPlayerBodyRadius = 0.18f;
// Roughly a head: large enough that the near clip plane never pokes through
// a wall, small enough that the player can put their face up to one.
constexpr float kHeadRadius = 0.12f;

// How long a corpse stays when the asset has no death animation to time
// against. Long enough to read as "that one died", short enough that a wave
// shooter does not accumulate a field of statues.
constexpr float kDefaultDeathLinger = 1.2f;

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
        // World, not frame-local: the two are the same only while the player
        // is unparented, and every caller of this speaks world space.
        return app_->rig_world_position();
    }
    float yaw_radians() const override { return app_->rig_world_yaw(); }

    void translate(const Vec3& delta) override {
        if (!is_finite(delta)) {
            PTV_WARN_CLAMP_ONCE("locomotion: rejected non-finite translate");
            return;
        }
        Vec3 d = delta;
        // Grounded is the default, and in it vertical position belongs to the
        // ground. A slide mechanic that forwarded the Y of some sweep used to
        // lift the player a few millimetres a frame until they reached the
        // ceiling.
        if (app_->vertical_mode_ == ::ptv::VerticalMode::Grounded) d.y = 0.0f;
        const float len = glm::length(d);
        if (len > kMaxTranslatePerCall) {
            PTV_WARN_CLAMP_ONCE(
                "locomotion: translate of {:.1f} m clamped to {:.1f} m", len,
                kMaxTranslatePerCall);
            d *= (kMaxTranslatePerCall / len);
        }
        // Requested, not applied: the motor resolves every request made this
        // frame in one sweep, after all locomotion mechanics have run.
        app_->pending_move_ += d;
    }

    void move_horizontal(const Vec3& delta) override {
        translate(Vec3(delta.x, 0.0f, delta.z));
    }

    bool is_grounded() const override {
        if (!app_->motor_active()) return true;
        return app_->physics_.player_character_state().grounded;
    }

    void jump(float speed_mps) override {
        if (!std::isfinite(speed_mps) || speed_mps <= 0.0f) return;
        if (app_->vertical_mode_ != ::ptv::VerticalMode::Grounded) return;
        app_->pending_jump_mps_ =
            std::max(app_->pending_jump_mps_, std::min(speed_mps, 12.0f));
    }

    void set_vertical_mode(::ptv::VerticalMode mode) override {
        app_->vertical_mode_ = mode;
    }
    ::ptv::VerticalMode vertical_mode() const override {
        return app_->vertical_mode_;
    }

    Vec3 body_position() const override {
        if (app_->motor_active()) {
            return app_->physics_.player_character_state().feet;
        }
        const Vec3 head = app_->current_head_world();
        return Vec3(head.x, app_->rig_world_position().y, head.z);
    }

    Vec3 head_position() const override { return app_->current_head_world(); }

    Quat head_rotation() const override {
        const Quat yaw = glm::angleAxis(app_->rig_world_yaw(), Vec3(0, 1, 0));
        if (!app_->head_local_valid_) return yaw;
        return yaw * glm::quat_cast(Mat3(app_->head_local_));
    }

    ::ptv::TurnMode turn_mode() const override { return app_->turn_mode_; }
    void set_turn_mode(::ptv::TurnMode mode) override {
        app_->turn_mode_ = mode;
    }

    void declare_mode_available(::ptv::LocomotionMode mode) override {
        const auto i = static_cast<size_t>(mode);
        if (i < app_->mode_available_.size()) app_->mode_available_[i] = true;
    }
    bool mode_available(::ptv::LocomotionMode mode) const override {
        const auto i = static_cast<size_t>(mode);
        return i < app_->mode_available_.size() && app_->mode_available_[i];
    }
    void declare_turn_available(::ptv::TurnMode mode) override {
        const auto i = static_cast<size_t>(mode);
        if (i < app_->turn_available_.size()) app_->turn_available_[i] = true;
    }
    bool turn_available(::ptv::TurnMode mode) const override {
        const auto i = static_cast<size_t>(mode);
        return i < app_->turn_available_.size() && app_->turn_available_[i];
    }

    void set_input_lock(const char* owner, bool locked) override {
        const std::string id = owner == nullptr ? std::string() : owner;
        auto& owners = app_->input_lock_owners_;
        const auto it = std::find(owners.begin(), owners.end(), id);
        if (locked && it == owners.end()) owners.push_back(id);
        if (!locked && it != owners.end()) owners.erase(it);
    }

    void rotate_yaw(float radians) override {
        if (!std::isfinite(radians)) {
            PTV_WARN_CLAMP_ONCE("locomotion: rejected non-finite rotate_yaw");
            return;
        }
        // A single call should never spin more than half a turn.
        constexpr float kPi = 3.14159265358979323846f;
        const float r = std::max(-kPi, std::min(kPi, radians));
        // Turn about the HEAD, not the stage origin. Rotating the rig about
        // its origin swings a player standing a metre off-centre in their
        // playspace through a one-metre arc on every snap -- the view jumps
        // sideways as well as turning.
        const Vec3 before = app_->current_head_world();
        app_->rig_.yaw_rad += r;
        const Vec3 after = app_->current_head_world();
        const Vec3 fix = app_->world_vector_to_frame(
            Vec3(before.x - after.x, 0.0f, before.z - after.z));
        app_->rig_.pos_x += fix.x;
        app_->rig_.pos_z += fix.z;
    }

    void teleport_to(const Vec3& target) override {
        if (!is_finite(target)) {
            PTV_WARN_CLAMP_ONCE("locomotion: rejected non-finite teleport_to");
            return;
        }
        Vec3 dst = target;
        // Cap horizontal distance to what the scene allows. Both the target
        // and the comparison are in WORLD space -- a teleport aims at a
        // raycast landing spot, which physics reported in world.
        const Vec3 here = app_->rig_world_position();
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
        // Land the player's FEET on the target. Moving the stage origin there
        // instead leaves a player who stands off-centre in their playspace
        // that far away from where they aimed.
        const Vec3 head = app_->current_head_world();
        const Vec3 rig = app_->rig_world_position();
        const Vec3 offset(head.x - rig.x, 0.0f, head.z - rig.z);
        app_->set_rig_world_position(
            Vec3(dst.x - offset.x, dst.y, dst.z - offset.z));
        app_->pending_move_ = Vec3(0.0f);
        if (app_->motor_active()) {
            app_->physics_.teleport_player_character(dst);
            app_->motor_head_resolved_ = app_->current_head_world();
        }
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
        // Whatever menu is open owns the sticks.
        return !app_->input_lock_owners_.empty();
    }

    const LocomotionTunables& tunables() const override {
        // Refreshed from the scene's player block each call so a mechanic
        // always sees the values the scene actually authored.
        tunables_.slide_max_speed_mps = app_->slide_max_speed_mps_;
        tunables_.slide_accel_tau_s = app_->slide_accel_tau_s_;
        tunables_.slide_decel_tau_s = app_->slide_decel_tau_s_;
        tunables_.sprint_speed_mps = app_->sprint_speed_mps_;
        tunables_.sprint_allowed = app_->scene_.sprint_activation != "off";
        tunables_.sprint_accel_tau_s = app_->sprint_accel_tau_s_;
        tunables_.sprint_decel_tau_s = app_->sprint_decel_tau_s_;
        tunables_.sprint_forward_cone_deg = app_->sprint_forward_cone_deg_;
        tunables_.sprint_min_stick = app_->sprint_min_stick_;
        tunables_.teleport_launch_speed_mps = app_->teleport_launch_speed_mps_;
        tunables_.teleport_max_horizontal_m = app_->teleport_max_horizontal_m_;
        tunables_.teleport_floor_y = app_->teleport_floor_y_;
        tunables_.snap_turn_degrees = app_->snap_turn_degrees_;
        tunables_.snap_turn_cooldown_s = app_->snap_turn_cooldown_s_;
        return tunables_;
    }

    bool sprinting() const override { return app_->sprinting_; }
    float gait_speed_cap() const override { return app_->gait_speed_cap_; }
    void publish_sprint(bool sprinting, float speed_cap) override {
        app_->sprinting_ = sprinting;
        app_->gait_speed_cap_ =
            std::isfinite(speed_cap) ? std::max(0.0f, speed_cap)
                                     : app_->slide_max_speed_mps_;
    }
    ::ptv::SprintActivation sprint_activation() const override {
        return app_->sprint_activation_;
    }
    void set_sprint_activation(::ptv::SprintActivation mode) override {
        app_->sprint_activation_ = mode;
    }

    void set_reference_frame(std::uint32_t entity,
                             const char* anchor) override {
        app_->set_reference_frame(entity,
                                  anchor == nullptr ? "" : std::string(anchor));
    }

    void clear_reference_frame() override { app_->clear_reference_frame(); }

    std::uint32_t reference_frame() const override {
        return app_->has_reference_frame()
                   ? static_cast<std::uint32_t>(app_->rig_.parent)
                   : InputSnapshot::kInvalidEntity;
    }

    void set_player_scale(float scale) override {
        if (!std::isfinite(scale) || scale <= 0.0f) {
            PTV_WARN("set_player_scale: rejecting non-finite/non-positive {}",
                     scale);
            return;
        }
        app_->set_player_scale(scale);
    }

    float player_scale() const override { return app_->rig_.scale; }

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
        std::uint32_t entity = InputSnapshot::kInvalidEntity;
        if (!app_->physics_.raycast(origin, dir, max_dist, hit, normal,
                                    entity)) {
            return false;
        }
        out.point = hit;
        out.normal = normal;
        out.entity = entity;
        out.hit = true;
        return true;
    }

    bool raycast_excluding(std::uint32_t self, const Vec3& origin,
                           const Vec3& dir, float max_dist, PhysicsRayHit& out,
                           bool floor_only) const override {
        out = PhysicsRayHit{};
        if (!is_finite(origin) || !is_finite(dir) || !std::isfinite(max_dist)) {
            return false;
        }
        PhysicsWorld::RayFilter filter{};
        if (self != InputSnapshot::kInvalidEntity) filter.ignore.push_back(self);
        filter.skip_characters = floor_only;
        Vec3 hit(0.0f);
        Vec3 normal(0.0f, 1.0f, 0.0f);
        std::uint32_t entity = InputSnapshot::kInvalidEntity;
        if (!app_->physics_.raycast(origin, dir, max_dist, filter, hit, normal,
                                    entity)) {
            return false;
        }
        out.point = hit;
        out.normal = normal;
        out.entity = entity;
        out.hit = true;
        return true;
    }

    bool raycast_floor(std::uint32_t self, const Vec3& origin, const Vec3& dir,
                       float max_dist, PhysicsRayHit& out) const override {
        out = PhysicsRayHit{};
        if (!is_finite(origin) || !is_finite(dir) || !std::isfinite(max_dist)) {
            return false;
        }
        PhysicsWorld::RayFilter filter{};
        if (self != InputSnapshot::kInvalidEntity) filter.ignore.push_back(self);
        filter.floor_only = true;
        Vec3 hit(0.0f);
        Vec3 normal(0.0f, 1.0f, 0.0f);
        std::uint32_t entity = InputSnapshot::kInvalidEntity;
        if (!app_->physics_.raycast(origin, dir, max_dist, filter, hit, normal, entity)) {
            return false;
        }
        out.point = hit;
        out.normal = normal;
        out.entity = entity;
        out.hit = true;
        return true;
    }

    bool raycast_ignoring(const std::uint32_t* ignore, int count, const Vec3& origin,
                          const Vec3& dir, float max_dist,
                          PhysicsRayHit& out) const override {
        return sphere_cast_ignoring(ignore, count, origin, dir, 0.0f, max_dist, out);
    }

    bool sphere_cast_ignoring(const std::uint32_t* ignore, int count, const Vec3& origin,
                              const Vec3& dir, float radius, float max_dist,
                              PhysicsRayHit& out) const override {
        out = PhysicsRayHit{};
        if (!is_finite(origin) || !is_finite(dir) || !std::isfinite(max_dist) ||
            !std::isfinite(radius)) {
            return false;
        }
        PhysicsWorld::RayFilter filter{};
        for (int i = 0; ignore != nullptr && i < count; ++i) {
            if (ignore[i] != InputSnapshot::kInvalidEntity) filter.ignore.push_back(ignore[i]);
        }
        Vec3 hit(0.0f);
        Vec3 normal(0.0f, 1.0f, 0.0f);
        std::uint32_t entity = InputSnapshot::kInvalidEntity;
        const bool ok = radius > 1e-4f
            ? app_->physics_.sphere_cast(origin, dir, radius, max_dist, filter, hit,
                                         normal, entity)
            : app_->physics_.raycast(origin, dir, max_dist, filter, hit, normal, entity);
        if (!ok) return false;
        out.point = hit;
        out.normal = normal;
        out.entity = entity;
        out.hit = true;
        return true;
    }

    bool has_body(std::uint32_t entity) const override {
        return app_->physics_.has_body(entity);
    }

    float distance_to_body(std::uint32_t entity, const Vec3& point) const override {
        if (!is_finite(point)) return -1.0f;
        return app_->physics_.distance_to_body(entity, point);
    }

    void set_collision_enabled(std::uint32_t entity, bool enabled) override {
        // A death (begin_death) destroys the body outright; something that
        // comes back -- a restored target -- gets a fresh one.
        if (enabled && !app_->physics_.has_body(entity) && app_->world_.valid(entity) &&
            app_->world_.has<CPhysics>(entity)) {
            app_->physics_.add_body_for(app_->world_, entity);
            return;
        }
        app_->physics_.set_collision_enabled(entity, enabled);
    }

    bool body_center(std::uint32_t entity, Vec3* out) const override {
        return app_->physics_.body_center(entity, out);
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

    void teleport_body(std::uint32_t entity, const Vec3& pos, const Quat& rot) override {
        if (!is_finite(pos) || !is_finite(rot)) {
            PTV_WARN_CLAMP_ONCE("physics: rejected non-finite teleport_body");
            return;
        }
        app_->physics_.teleport(entity, pos, rot);
        if (app_->world_.has<CTransform>(entity)) {
            auto& t = app_->world_.get<CTransform>(entity).t;
            t.position = pos;
            t.rotation = rot;
        }
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

    Vec3 slide_move(std::uint32_t entity, const Vec3& delta) const override {
        if (!is_finite(delta)) {
            PTV_WARN_CLAMP_ONCE("physics: rejected non-finite slide_move");
            return Vec3(0.0f);
        }
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CTransform>(entity)) return delta;
        const auto& t = w.get<CTransform>(entity).t;

        // Same height rule PhysicsWorld uses to build the capsule, so the
        // sweep shape matches the collider the entity actually has.
        float height = std::max(0.2f, t.scale);
        if (w.has<CMesh>(entity)) {
            const auto& m = w.get<CMesh>(entity);
            if (m.target_scale_meters > 1.0e-4f) {
                height = m.target_scale_meters;
            } else if (m.bounds_radius_m > 1.0e-4f) {
                height = 2.0f * m.bounds_radius_m;
            }
        }
        return app_->physics_.slide_character(t.position, height, delta,
                                              entity);
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

class Application::AppService final : public IApp {
public:
    explicit AppService(Application* app) : app_(app) {}
    void request_exit() override { app_->request_exit(); }
    bool exit_requested() const override { return app_->exit_requested_; }
    const char* platform() const override {
#if defined(__ANDROID__)
        return "quest";
#else
        return "pcvr";
#endif
    }
    bool verify_flow() const override { return app_->verify_flow_; }
    bool verify_run() const override { return app_->verify_mode_; }
    float uptime_s() const override { return static_cast<float>(app_->uptime_s_); }
    void request_fade(float amount) override {
        if (!std::isfinite(amount)) return;
        app_->fade_request_ = std::max(app_->fade_request_, std::clamp(amount, 0.0f, 1.0f));
    }
    void request_flash(float amount, const Vec3& color) override {
        if (!std::isfinite(amount) || !is_finite(color)) return;
        const float a = std::clamp(amount, 0.0f, 1.0f);
        if (a > app_->flash_request_) {
            app_->flash_request_ = a;
            app_->flash_color_ = glm::clamp(color, Vec3(0.0f), Vec3(1.0f));
        }
    }
    bool place_player(const Vec3& feet, float yaw) override {
        if (!std::isfinite(feet.x) || !std::isfinite(feet.y) ||
            !std::isfinite(feet.z) || !std::isfinite(yaw)) {
            return false;
        }
        if (app_->player_entity_ == entt::null) return false;
        app_->set_rig_world_position(feet);
        app_->set_rig_world_yaw(yaw);
        // Stand the body under the head again on the next pose, as at load.
        app_->motor_needs_sync_ = true;
        app_->pending_move_ = Vec3(0.0f);
        return true;
    }
    Vec3 scene_spawn_position() const override { return app_->scene_.spawn.position; }
    float scene_spawn_yaw() const override {
        const Vec3 f = glm::mat3_cast(app_->scene_.spawn.rotation) * Vec3(0.0f, 0.0f, -1.0f);
        return yaw_from_forward(f);
    }

private:
    Application* app_;
};

class Application::TextDrawService final : public ITextDraw {
public:
    explicit TextDrawService(Application* app) : app_(app) {}
    void text(const char* utf8, const Vec3& position, const Quat& rotation,
              float height_m, const Vec4& color, TextAlign align) override {
        if (utf8 == nullptr || *utf8 == '\0' || !std::isfinite(height_m) ||
            height_m <= 0.0f || !std::isfinite(position.x) ||
            !std::isfinite(position.y) || !std::isfinite(position.z)) {
            return;
        }
        TextItem it{};
        it.text = utf8;
        it.position = position;
        it.rotation = rotation;
        it.height_m = std::clamp(height_m, 0.002f, 5.0f);
        it.color = color;
        it.align = static_cast<int>(align);
        app_->renderer_.submit_text(std::move(it));
    }
    float measure(const char* utf8, float height_m) const override {
        if (utf8 == nullptr) return 0.0f;
        if (const TextAtlas* a = app_->renderer_.text_atlas()) {
            return a->measure(utf8, height_m);
        }
        return 0.6f * height_m * static_cast<float>(std::strlen(utf8));
    }

private:
    Application* app_;
};

class Application::ComfortService final : public IComfort {
public:
    explicit ComfortService(Application* app) : app_(app) {}

    void offer_vignette(ComfortChannel channel, bool enabled,
                        bool user_toggle) override {
        auto* c = get(channel);
        if (c == nullptr) return;
        if (!c->offered) c->enabled = enabled;
        c->offered = true;
        c->toggleable = c->toggleable || user_toggle;
    }
    bool vignette_offered(ComfortChannel channel) const override {
        const auto* c = get(channel);
        return c != nullptr && c->offered;
    }
    bool vignette_toggleable(ComfortChannel channel) const override {
        const auto* c = get(channel);
        return c != nullptr && c->offered && c->toggleable;
    }
    bool vignette_enabled(ComfortChannel channel) const override {
        const auto* c = get(channel);
        return c != nullptr && c->offered && c->enabled;
    }
    void set_vignette_enabled(ComfortChannel channel, bool on) override {
        auto* c = get(channel);
        if (c != nullptr && c->offered) c->enabled = on;
    }

    void request_vignette(ComfortChannel channel, float strength,
                          float inner_fov_deg, float feather_deg) override {
        if (!vignette_enabled(channel)) return;
        if (!std::isfinite(strength) || !std::isfinite(inner_fov_deg) ||
            !std::isfinite(feather_deg)) {
            return;
        }
        const float s = std::clamp(strength, 0.0f, 1.0f);
        if (s <= app_->vignette_request_) return;
        app_->vignette_request_ = s;
        app_->vignette_request_inner_deg_ =
            std::clamp(inner_fov_deg, 20.0f, 100.0f);
        app_->vignette_request_feather_deg_ =
            std::clamp(feather_deg, 2.0f, 60.0f);
    }

    void reset() { app_->vignette_channels_ = {}; }

private:
    Application::VignetteChannel* get(ComfortChannel c) {
        const auto i = static_cast<size_t>(c);
        return i < app_->vignette_channels_.size()
                   ? &app_->vignette_channels_[i]
                   : nullptr;
    }
    const Application::VignetteChannel* get(ComfortChannel c) const {
        const auto i = static_cast<size_t>(c);
        return i < app_->vignette_channels_.size()
                   ? &app_->vignette_channels_[i]
                   : nullptr;
    }

    Application* app_;
};

void Application::tick_comfort_vignette(float dt) {
    // Meta's LocomotionTunneling behaviour: follow the motion quickly, hold
    // briefly once it stops (FadeOutWait), then fade out (FadeOutTime). A
    // vignette that snapped off the instant the stick centred would itself
    // be a sudden change in the visual field.
    constexpr float kAttackTau = 0.08f;
    constexpr float kFadeOutWait = 0.15f;
    constexpr float kReleaseTau = 0.25f;
    const float target = vignette_request_;
    if (target > 0.0f) {
        vignette_since_request_s_ = 0.0f;
        vignette_inner_deg_ = vignette_request_inner_deg_;
        vignette_feather_deg_ = vignette_request_feather_deg_;
    } else {
        vignette_since_request_s_ += dt;
    }
    if (target >= vignette_level_) {
        vignette_level_ +=
            (target - vignette_level_) * (1.0f - std::exp(-dt / kAttackTau));
    } else if (vignette_since_request_s_ > kFadeOutWait || target > 0.0f) {
        vignette_level_ +=
            (target - vignette_level_) * (1.0f - std::exp(-dt / kReleaseTau));
    }
    if (vignette_level_ < 1e-3f) vignette_level_ = 0.0f;
    vignette_request_ = 0.0f;

    // The clear field narrows from beyond the edge of the display (no
    // occlusion) toward the requested inner FOV as the level rises, and the
    // edge fades in rather than popping.
    constexpr float kOpenHalfAngle = 75.0f;
    const float clear =
        kOpenHalfAngle +
        (0.5f * vignette_inner_deg_ - kOpenHalfAngle) * vignette_level_;
    const float opacity = std::min(1.0f, vignette_level_ * 4.0f) * 0.95f;

    // A gameplay fade (respawn, restart) is a whole-view fade to black and
    // wins over the comfort vignette when stronger. Smoothed so it never
    // pops, quickly enough that a 0.5 s fade reads as one.
    fade_level_ += (fade_request_ - fade_level_) * (1.0f - std::exp(-dt / 0.12f));
    if (fade_level_ < 1e-3f) fade_level_ = 0.0f;
    fade_request_ = 0.0f;
    // A flash (a flashbang's white-out) hits at once and eases out as the
    // requester decays it.
    flash_level_ = flash_request_ > flash_level_
                       ? flash_request_
                       : flash_level_ + (flash_request_ - flash_level_) *
                                            (1.0f - std::exp(-dt / 0.08f));
    if (flash_level_ < 1e-3f) flash_level_ = 0.0f;
    flash_request_ = 0.0f;
    if (flash_level_ > fade_level_ && flash_level_ > opacity) {
        renderer_.set_vignette(flash_level_, 0.0f, 1.0f, flash_color_);
        return;
    }
    if (fade_level_ > opacity) {
        // Clear half-angle 0 with a narrow feather covers the whole view.
        renderer_.set_vignette(fade_level_, 0.0f, 1.0f);
        return;
    }
    renderer_.set_vignette(opacity, clear, vignette_feather_deg_);
}

void Application::check_player_mechanic_roles() const {
    // The engine injects NOTHING. Every player mechanic -- locomotion,
    // turning, the settings menu, hand posing, the avatar body -- is bound by
    // the scene, and a scene that binds none of them is a stationary game.
    //
    // What it still does is refuse to be silent about two bindings doing the
    // same job, which is how the double teleport arc and the double snap
    // turn shipped. The pipeline rejects that at compile time; this is the
    // defence in depth for hand-authored scenes.
    static const std::pair<const char*, const char*> kRoles[] = {
        {"builtin.locomotion.teleport", "role:teleport"},
        {"builtin.teleport", "role:teleport"},
        {"builtin.locomotion.slide.head_relative", "role:slide_head"},
        {"builtin.locomotion.slide.controller_relative",
         "role:slide_controller"},
        {"builtin.locomotion.snap_turn", "role:snap_turn"},
        {"builtin.snap_turn", "role:snap_turn"},
        {"builtin.locomotion.smooth_turn", "role:smooth_turn"},
        {"builtin.movement_settings_menu", "role:movement_settings_menu"},
        {"builtin.hand_pose", "role:fingers"},
        {"builtin.avatar_body", "role:body"},
    };
    std::unordered_map<std::string, std::vector<std::string>> by_role;
    auto view = world_.registry().view<const CMechanicBinding>();
    for (auto e : view) {
        for (const auto& entry : view.get<const CMechanicBinding>(e).entries) {
            const std::string& id =
                entry.builtin_id.empty() ? entry.id : entry.builtin_id;
            for (const auto& [builtin, role] : kRoles) {
                if (id == builtin) by_role[role].push_back(id);
            }
        }
    }
    for (const auto& [role, ids] : by_role) {
        if (ids.size() < 2) continue;
        PTV_ERROR(
            "Scene binds {} mechanics for {} ({} and {}...). Only one may own "
            "a player role; expect doubled input handling.",
            ids.size(), role, ids[0], ids[1]);
    }
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

    std::uint32_t hand_entity(int hand) const override {
        if (hand < 0 || hand > 1) return InputSnapshot::kInvalidEntity;
        const auto e = app_->hand_entities_[hand];
        return e == entt::null ? InputSnapshot::kInvalidEntity
                               : static_cast<std::uint32_t>(e);
    }

    std::uint32_t finger_target(int hand) const override {
        if (hand < 0 || hand > 1) return InputSnapshot::kInvalidEntity;
        const auto e = app_->finger_targets_[static_cast<size_t>(hand)];
        if (e == entt::null || !app_->world_.registry().valid(e)) {
            return InputSnapshot::kInvalidEntity;
        }
        return static_cast<std::uint32_t>(e);
    }

    void set_finger_target(int hand, std::uint32_t entity) override {
        if (hand < 0 || hand > 1) return;
        app_->finger_targets_[static_cast<size_t>(hand)] =
            (entity == InputSnapshot::kInvalidEntity)
                ? entt::null
                : static_cast<entt::entity>(entity);
    }

    bool fingertip_world(int hand, const char* finger, Vec3* out) const override {
        if (hand < 0 || hand > 1 || out == nullptr) return false;
        const std::string f = (finger != nullptr && *finger) ? finger : "index";
        const std::string side = hand == 0 ? "_l" : "_r";
        const std::uint32_t skel = finger_target(hand);
        const ISkeleton* sk = app_->mech_ctx_.skeleton;
        if (sk != nullptr && skel != InputSnapshot::kInvalidEntity && sk->has_skeleton(skel)) {
            const int j3 = sk->joint_for_slot(skel, (f + "_3" + side).c_str());
            const int j2 = sk->joint_for_slot(skel, (f + "_2" + side).c_str());
            if (j3 >= 0 && j2 >= 0) {
                const Vec3 a(sk->joint_world(skel, j2)[3]);
                const Vec3 b(sk->joint_world(skel, j3)[3]);
                // The distal joint is the last knuckle; the tip is about one
                // middle-phalanx length past it.
                *out = b + 0.85f * (b - a);
                return is_finite(*out);
            }
        }
        // No posable fingers: an index tip ~9.5 cm ahead of the grip pose.
        const auto he = app_->hand_entities_[static_cast<size_t>(hand)];
        if (he != entt::null && app_->world_.registry().all_of<CTransform>(he)) {
            const auto& t = app_->world_.registry().get<CTransform>(he).t;
            *out = t.position + t.rotation * Vec3(0.0f, -0.01f, -0.095f);
            return true;
        }
        return false;
    }

    std::uint32_t player_entity() const override {
        const auto e = app_->player_entity_;
        return e == entt::null ? InputSnapshot::kInvalidEntity
                               : static_cast<std::uint32_t>(e);
    }

    void set_hand_anchor(int hand, const Vec3& grip_position,
                         const Quat& grip_rotation, float weight) override {
        if (hand < 0 || hand > 1) return;
        if (!std::isfinite(grip_position.x) || !std::isfinite(grip_position.y) ||
            !std::isfinite(grip_position.z)) {
            return;
        }
        auto& a = app_->hand_anchors_[static_cast<size_t>(hand)];
        a.frame = app_->frame_index_;
        a.position = grip_position;
        a.rotation = glm::normalize(grip_rotation);
        a.weight = std::clamp(weight, 0.0f, 1.0f);
    }

    bool hand_anchor(int hand, Vec3* grip_position, Quat* grip_rotation,
                     float* weight) const override {
        if (hand < 0 || hand > 1) return false;
        const auto& a = app_->hand_anchors_[static_cast<size_t>(hand)];
        if (a.frame != app_->frame_index_ || a.weight <= 0.0f) return false;
        if (grip_position) *grip_position = a.position;
        if (grip_rotation) *grip_rotation = a.rotation;
        if (weight) *weight = a.weight;
        return true;
    }

    Vec3 forearm_direction(int hand) const override {
        if (hand < 0 || hand > 1) return Vec3(0.0f, 0.0f, 1.0f);
        const auto h = static_cast<size_t>(hand);
        // The avatar's own forearm, when a body published one recently.
        if (app_->forearm_frame_[h] + 2 >= app_->frame_index_) {
            return app_->forearm_dir_[h];
        }
        return app_->hand_forearm_direction(hand);
    }

    void publish_forearm_direction(int hand, const Vec3& dir) override {
        if (hand < 0 || hand > 1) return;
        const float len = glm::length(dir);
        if (!(len > 1e-5f) || !std::isfinite(len)) return;
        app_->forearm_dir_[static_cast<size_t>(hand)] = dir / len;
        app_->forearm_frame_[static_cast<size_t>(hand)] = app_->frame_index_;
    }

    std::uint32_t support_entity(int hand) const override {
        if (hand < 0 || hand > 1) return InputSnapshot::kInvalidEntity;
        return app_->support_entity_[static_cast<size_t>(hand)];
    }

    void set_support_entity(int hand, std::uint32_t entity) override {
        if (hand < 0 || hand > 1) return;
        app_->support_entity_[static_cast<size_t>(hand)] = entity;
    }

    void request_hold(int hand, std::uint32_t entity) override {
        if (hand < 0 || hand > 1) return;
        app_->hold_request_[static_cast<size_t>(hand)] = entity;
    }

    std::uint32_t take_hold_request(int hand) override {
        if (hand < 0 || hand > 1) return InputSnapshot::kInvalidEntity;
        const auto e = app_->hold_request_[static_cast<size_t>(hand)];
        app_->hold_request_[static_cast<size_t>(hand)] = InputSnapshot::kInvalidEntity;
        return e;
    }

private:
    Application* app_;
};

class Application::ContactsService final : public IContacts {
public:
    explicit ContactsService(Application* app) : app_(app) {}

    bool touching(std::uint32_t a, std::uint32_t b) const override {
        if (a == b) return false;
        const auto& list = overlaps_for(a);
        return std::find(list.begin(), list.end(), b) != list.end();
    }

    bool began(std::uint32_t a, std::uint32_t b) override {
        const bool now = touching(a, b);
        // Order-independent key so began(a,b) and began(b,a) share one edge.
        const std::uint64_t key = (a < b)
            ? (static_cast<std::uint64_t>(a) << 32) | b
            : (static_cast<std::uint64_t>(b) << 32) | a;
        auto it = pair_state_.find(key);
        const bool before = it != pair_state_.end() && it->second;
        pair_state_[key] = now;
        return now && !before;
    }

    int overlapping(std::uint32_t entity, std::uint32_t* out,
                    int max_out) const override {
        if (out == nullptr || max_out <= 0) return 0;
        const auto& list = overlaps_for(entity);
        const int n = std::min<int>(max_out, static_cast<int>(list.size()));
        for (int i = 0; i < n; ++i) out[i] = list[static_cast<size_t>(i)];
        return n;
    }

    /** Drop the frame's memoised overlap sets. Pair edges deliberately
     *  survive: they are what ``began`` compares against. */
    void begin_frame() { cache_.clear(); }

private:
    /** One narrow-phase query per entity per frame, reused by every caller.
     *  Ten enemies all asking "am I touching the player?" would otherwise be
     *  ten separate broad-phase walks. */
    const std::vector<std::uint32_t>& overlaps_for(std::uint32_t e) const {
        auto it = cache_.find(e);
        if (it != cache_.end()) return it->second;
        auto& slot = cache_[e];
        app_->physics_.overlapping(e, slot);
        return slot;
    }

    Application* app_;
    mutable std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>
        cache_{};
    std::unordered_map<std::uint64_t, bool> pair_state_{};
};

class Application::SpawnService final : public ISpawn {
public:
    explicit SpawnService(Application* app) : app_(app) {}

    std::uint32_t instance(std::uint32_t template_entity, const Vec3& position,
                           const Quat& rotation) override {
        if (!is_finite(position) || !is_finite(rotation)) {
            PTV_WARN_CLAMP_ONCE("spawn: rejected non-finite pose");
            return InputSnapshot::kInvalidEntity;
        }
        if (static_cast<int>(spawned_.size()) >= kMaxLiveSpawns) {
            PTV_WARN_CLAMP_ONCE(
                "spawn: {} live spawns is the cap; refusing more",
                kMaxLiveSpawns);
            return InputSnapshot::kInvalidEntity;
        }
        // Animated characters get their own, much tighter budget. Posing a
        // skeleton is CPU work that competes with the render thread, so a
        // wave spawner left to its own devices is the fastest way to drop a
        // Quest below frame rate -- and it will look like a rendering
        // problem rather than a spawning one.
        if (app_->world_.valid(template_entity) &&
            app_->world_.has<CSkeleton>(template_entity)) {
            const int cap = std::max(0, app_->scene_.performance
                                              .max_animated_agents);
            if (live_animated() >= cap) {
                PTV_WARN_CLAMP_ONCE(
                    "spawn: {} animated agents is this scene's budget; "
                    "refusing more until some die",
                    cap);
                return InputSnapshot::kInvalidEntity;
            }
        }
        const auto created =
            app_->clone_entity(template_entity, position, rotation);
        if (created == entt::null) return InputSnapshot::kInvalidEntity;
        const auto id = static_cast<std::uint32_t>(created);
        spawned_[id] = template_entity;
        const std::string* tname =
            app_->world_.id_of(static_cast<entt::entity>(template_entity));
        PTV_INFO("spawn: {} -> entity {} at {:.2f},{:.2f},{:.2f}",
                 tname ? *tname : std::string("?"), id, position.x, position.y,
                 position.z);
        return id;
    }

    void despawn(std::uint32_t entity) override {
        app_->destroy_entity_runtime(entity);
        spawned_.erase(entity);
    }

    /** Drop bookkeeping for an entity the engine destroyed on its own (a
     *  death, say) so ``live_count`` does not keep counting corpses. */
    void forget(std::uint32_t entity) { spawned_.erase(entity); }

    int live_count(std::uint32_t template_entity) const override {
        int n = 0;
        for (const auto& [child, parent] : spawned_) {
            if (parent != template_entity) continue;
            if (!app_->world_.valid(child)) continue;
            ++n;
        }
        return n;
    }

private:
    /** How many live clones currently carry a skeleton. */
    int live_animated() const {
        int n = 0;
        for (const auto& [child, parent] : spawned_) {
            (void)parent;
            if (!app_->world_.valid(child)) continue;
            if (!app_->world_.has<CSkeleton>(child)) continue;
            ++n;
        }
        return n;
    }

    // A runaway spawner is the classic way a generated game drops to single
    // digit frames on a Quest. Cap it here so the worst case is "enemies stop
    // appearing", not "the headset stutters".
    static constexpr int kMaxLiveSpawns = 256;

    Application* app_;
    std::unordered_map<std::uint32_t, std::uint32_t> spawned_{};
};

class Application::AttachService final : public IAttach {
public:
    explicit AttachService(Application* app) : app_(app) {}

    bool attach(std::uint32_t child, std::uint32_t parent, const char* anchor,
                const Vec3& local_position,
                const Quat& local_rotation) override {
        World& w = app_->world_;
        if (child == parent) return false;
        if (!w.valid(child) || !w.valid(parent)) return false;
        if (!w.has<CTransform>(child) || !w.has<CTransform>(parent)) {
            return false;
        }
        if (!is_finite(local_position) || !is_finite(local_rotation)) {
            PTV_WARN_CLAMP_ONCE("attach: rejected non-finite offset");
            return false;
        }
        const std::string name = anchor == nullptr ? std::string() : anchor;
        if (!app_->resolve_attach_anchor(parent, name).valid) {
            PTV_WARN("attach: '{}' is neither a socket nor a joint on entity "
                     "{}", name, parent);
            return false;
        }

        CAttachment a{};
        a.parent = parent;
        a.anchor = name;
        a.local_position = local_position;
        a.local_rotation = glm::normalize(local_rotation);
        w.emplace_or_replace<CAttachment>(child, std::move(a));

        // The parent owns the child's pose from now on, so simulating it
        // would just fight the attachment every frame.
        if (app_->physics_.has_body(child)) {
            app_->physics_.set_kinematic(child, true);
        }
        return true;
    }

    bool attach_by_socket(std::uint32_t child, const char* child_socket,
                          std::uint32_t parent, const char* anchor) override {
        World& w = app_->world_;
        if (!w.valid(child) || !w.valid(parent) || child_socket == nullptr) return false;
        const auto ce = static_cast<entt::entity>(child);
        if (!w.registry().all_of<CMesh, CTransform>(ce)) return false;
        const auto& mesh = w.registry().get<CMesh>(ce);
        const auto it = mesh.sockets.find(child_socket);
        if (it == mesh.sockets.end()) {
            PTV_WARN("attach_by_socket: entity {} has no socket '{}'", child, child_socket);
            return false;
        }
        const float child_scale = w.registry().get<CTransform>(ce).t.scale;
        const Mat4 sock = glm::translate(Mat4(1.0f), it->second.position * child_scale) *
                          glm::mat4_cast(it->second.rotation);
        // The hand's grip frame relative to its wrist joint, measured from
        // the rig (palm centre + the rig's own grip alignment).
        Mat4 grip_in_anchor(1.0f);
        const std::string name = anchor == nullptr ? std::string() : anchor;
        const ISkeleton* skel_api = app_->mech_ctx_.skeleton;
        if (skel_api != nullptr &&
            w.registry().all_of<CSkeleton>(static_cast<entt::entity>(parent))) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool is_hand = lower.find("hand") != std::string::npos ||
                                 lower.find("wrist") != std::string::npos;
            if (is_hand) {
                const int side = (lower.find("_l") != std::string::npos ||
                                  lower.find("left") != std::string::npos)
                                     ? 0 : 1;
                const auto rig = hand_rig::analyze(*skel_api, parent, side);
                if (rig.valid) {
                    grip_in_anchor = glm::translate(Mat4(1.0f), rig.palm_offset_wrist_local) *
                                     glm::mat4_cast(glm::inverse(rig.grip_to_wrist_rot));
                }
            }
        }
        const Mat4 local = grip_in_anchor * glm::inverse(sock);
        return attach(child, parent, anchor, Vec3(local[3]),
                      glm::normalize(glm::quat_cast(Mat3(local))));
    }

    void detach(std::uint32_t child) override {
        World& w = app_->world_;
        if (!w.valid(child) || !w.has<CAttachment>(child)) return;
        w.remove<CAttachment>(child);
        // Hand it back to physics wherever it ended up. Grab does the same
        // thing on release, and for the same reason.
        if (app_->physics_.has_body(child)) {
            app_->physics_.set_kinematic(child, false);
        }
    }

    std::uint32_t parent_of(std::uint32_t child) const override {
        const World& w = app_->world_;
        if (!w.valid(child) || !w.has<CAttachment>(child)) {
            return InputSnapshot::kInvalidEntity;
        }
        return w.get<CAttachment>(child).parent;
    }

private:
    Application* app_;
};

class Application::InteractiveService final : public IInteractive {
public:
    explicit InteractiveService(Application* app) : app_(app) {}

    bool exists(const char* id) const override {
        return find(id) != nullptr;
    }

    std::uint32_t entity_of(const char* id) const override {
        if (id == nullptr) return InputSnapshot::kInvalidEntity;
        auto view = app_->world_.registry().view<const CInteractive>();
        for (auto e : view) {
            if (view.get<const CInteractive>(e).id == id) {
                return static_cast<std::uint32_t>(e);
            }
        }
        return InputSnapshot::kInvalidEntity;
    }

    const char* id_of(std::uint32_t entity) const override {
        const World& w = app_->world_;
        const auto e = static_cast<entt::entity>(entity);
        if (!w.valid(entity) || !w.registry().all_of<CInteractive>(e)) {
            return "";
        }
        return w.registry().get<CInteractive>(e).id.c_str();
    }

    const char* kind_of(const char* id) const override {
        const CInteractive* it = find(id);
        return it == nullptr ? "" : it->kind.c_str();
    }

    InteractiveValue get_state(const char* id,
                               const char* key) const override {
        const CInteractive* it = find(id);
        if (it == nullptr || key == nullptr) return InteractiveValue{};
        const auto found = it->state.find(key);
        // A missing key degrades to a default rather than failing: a mechanic
        // reading state its world did not declare should do nothing, not
        // crash the frame.
        return found == it->state.end() ? InteractiveValue{} : found->second;
    }

    void set_state(const char* id, const char* key,
                   const InteractiveValue& value) override {
        CInteractive* it = find_mutable(id);
        if (it == nullptr || key == nullptr) return;
        it->state[key] = value;
        // Nothing else to do: persistence is derived by diffing against
        // ``pristine`` at save time, so the mechanic that called this has
        // already finished its part.
    }

    bool is_changed(const char* id, const char* key) const override {
        const CInteractive* it = find(id);
        if (it == nullptr || key == nullptr) return false;
        const auto cur = it->state.find(key);
        const auto was = it->pristine.find(key);
        if (cur == it->state.end()) return false;
        if (was == it->pristine.end()) return true;  // key appeared at runtime
        return cur->second != was->second;
    }

    bool is_locked(const char* id) const override {
        const CInteractive* it = find(id);
        return it != nullptr && it->locked;
    }

    void set_locked(const char* id, bool locked) override {
        if (CInteractive* it = find_mutable(id)) it->locked = locked;
    }

    bool interaction_allowed(const char* id) const override {
        const CInteractive* it = find(id);
        // Unknown ids are permitted: the gate exists to express a DENIAL a
        // policy stated, not to make everything opt-in.
        return it == nullptr || it->interaction_allowed;
    }

    void set_interaction_allowed(const char* id, bool allowed) override {
        if (CInteractive* it = find_mutable(id)) {
            it->interaction_allowed = allowed;
        }
    }

    int find_by_kind(const char* kind, const char** out,
                     int max_out) const override {
        if (kind == nullptr || out == nullptr || max_out <= 0) return 0;
        int n = 0;
        auto view = app_->world_.registry().view<const CInteractive>();
        for (auto e : view) {
            if (n >= max_out) break;
            const auto& ci = view.get<const CInteractive>(e);
            if (ci.kind != kind) continue;
            out[n++] = ci.id.c_str();
        }
        return n;
    }

    /** Every id whose state has diverged from the document, with the deltas.
     *  Used by the save path; not part of the public interface because
     *  gameplay has no reason to serialize anything itself. */
    nlohmann::json changed_state() const {
        nlohmann::json out = nlohmann::json::object();
        auto view = app_->world_.registry().view<const CInteractive>();
        for (auto e : view) {
            const auto& ci = view.get<const CInteractive>(e);
            nlohmann::json deltas = nlohmann::json::object();
            for (const auto& [key, value] : ci.state) {
                const auto was = ci.pristine.find(key);
                if (was != ci.pristine.end() && was->second == value) continue;
                deltas[key] = to_json(value);
            }
            if (ci.locked) deltas["__locked"] = true;
            if (!deltas.empty()) out[ci.id] = std::move(deltas);
        }
        return out;
    }

    /** Re-apply saved deltas after a scene has been rebuilt from its
     *  document. The document is authoritative for everything else. */
    void apply_state(const nlohmann::json& saved) {
        if (!saved.is_object()) return;
        for (const auto& [id, deltas] : saved.items()) {
            CInteractive* it = find_mutable(id.c_str());
            if (it == nullptr || !deltas.is_object()) continue;
            for (const auto& [key, value] : deltas.items()) {
                if (key == "__locked") {
                    it->locked = value.is_boolean() ? value.get<bool>() : false;
                    continue;
                }
                InteractiveValue parsed{};
                if (!from_json(value, &parsed)) continue;
                it->state[key] = parsed;
            }
        }
    }

    static nlohmann::json to_json(const InteractiveValue& v) {
        switch (v.type) {
            case InteractiveValue::Type::Bool:
                return nlohmann::json{{"type", "bool"}, {"value", v.b}};
            case InteractiveValue::Type::Number:
                return nlohmann::json{{"type", "number"}, {"value", v.number}};
            case InteractiveValue::Type::Name:
                return nlohmann::json{{"type", "name"}, {"value", v.name}};
            case InteractiveValue::Type::Vector:
            default:
                return nlohmann::json{
                    {"type", "vector"},
                    {"value", nlohmann::json::array({v.vector.x, v.vector.y,
                                                     v.vector.z})}};
        }
    }

    static bool from_json(const nlohmann::json& j, InteractiveValue* out) {
        if (out == nullptr || !j.is_object() || !j.contains("type")) {
            return false;
        }
        const std::string type = j.value("type", std::string());
        if (type == "bool") {
            *out = InteractiveValue::of(j.value("value", false));
        } else if (type == "number") {
            *out = InteractiveValue::of(j.value("value", 0.0f));
        } else if (type == "name") {
            *out = InteractiveValue::named(j.value("value", std::string()));
        } else if (type == "vector") {
            const auto& a = j["value"];
            if (!a.is_array() || a.size() != 3) return false;
            *out = InteractiveValue::of(Vec3(a[0].get<float>(),
                                             a[1].get<float>(),
                                             a[2].get<float>()));
        } else {
            return false;
        }
        return true;
    }

private:
    const CInteractive* find(const char* id) const {
        if (id == nullptr || *id == '\0') return nullptr;
        auto view = app_->world_.registry().view<const CInteractive>();
        for (auto e : view) {
            const auto& ci = view.get<const CInteractive>(e);
            if (ci.id == id) return &ci;
        }
        return nullptr;
    }

    CInteractive* find_mutable(const char* id) {
        if (id == nullptr || *id == '\0') return nullptr;
        auto view = app_->world_.registry().view<CInteractive>();
        for (auto e : view) {
            auto& ci = view.get<CInteractive>(e);
            if (ci.id == id) return &ci;
        }
        return nullptr;
    }

    Application* app_;
};

class Application::UiService final : public IUi {
public:
    explicit UiService(Application* app) : app_(app) {}

    bool panel_exists(const char* panel_id) const override {
        return find(panel_id) != nullptr;
    }

    void open(const char* panel_id) override {
        if (CUiPanel* p = find(panel_id)) {
            if (!p->open) {
                p->open = true;
                // Re-anchor on open, and debounce: the trigger pull that
                // opened the panel must not also select whatever row happens
                // to be under the ray.
                p->anchor_set = false;
                p->click_cooldown_s = 0.35f;
            }
        }
    }

    void close(const char* panel_id) override {
        if (CUiPanel* p = find(panel_id)) {
            p->open = false;
            p->highlighted = -1;
            p->active_hand = -1;
            // Unread clicks belong to a panel that is no longer shown; keeping
            // them would fire an action after the player dismissed the menu.
            p->events.clear();
        }
    }

    bool is_open(const char* panel_id) const override {
        const CUiPanel* p = find(panel_id);
        return p != nullptr && p->open;
    }

    void set_rows(const char* panel_id, const UiRowDesc* rows,
                  int count) override {
        CUiPanel* p = find(panel_id);
        if (p == nullptr || rows == nullptr || count < 0) return;

        // Remember the highlight BY ID, not by index. A caller rebuilding the
        // list every frame (which is the simplest correct thing for it to do)
        // would otherwise reset or misplace the pointer on every rebuild.
        std::string highlighted_id;
        if (p->highlighted >= 0 &&
            p->highlighted < static_cast<int>(p->rows.size())) {
            highlighted_id = p->rows[static_cast<std::size_t>(p->highlighted)].id;
        }

        p->rows.clear();
        p->rows.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            UiRow row{};
            row.id = rows[i].id == nullptr ? "" : rows[i].id;
            row.label = rows[i].label == nullptr ? row.id : rows[i].label;
            row.thumbnail = rows[i].thumbnail;
            row.enabled = rows[i].enabled;
            row.selected = rows[i].selected;
            row.marked = rows[i].marked;
            p->rows.push_back(std::move(row));
        }

        p->highlighted = -1;
        if (!highlighted_id.empty()) {
            for (std::size_t i = 0; i < p->rows.size(); ++i) {
                if (p->rows[i].id == highlighted_id && p->rows[i].enabled) {
                    p->highlighted = static_cast<int>(i);
                    break;
                }
            }
        }
        if (p->highlighted < 0) p->active_hand = -1;
    }

    bool poll_click(const char* panel_id, UiClick* out) override {
        CUiPanel* p = find(panel_id);
        if (p == nullptr || p->events.empty()) return false;
        // Oldest first: clicks are an ordered sequence of player intent, and
        // acting on the newest first would reorder what they did.
        event_scratch_ = p->events.front();
        p->events.erase(p->events.begin());
        if (out != nullptr) {
            out->panel_id = event_scratch_.panel_id.c_str();
            out->row_id = event_scratch_.row_id.c_str();
            out->row_index = event_scratch_.row_index;
            out->hand = event_scratch_.hand;
        }
        return true;
    }

    int highlighted_row(const char* panel_id) const override {
        const CUiPanel* p = find(panel_id);
        return p == nullptr ? -1 : p->highlighted;
    }

    void set_title(const char* panel_id, const char* title,
                   const char* subtitle) override {
        if (CUiPanel* p = find(panel_id)) {
            p->title = title == nullptr ? "" : title;
            p->subtitle = subtitle == nullptr ? "" : subtitle;
        }
    }

private:
    CUiPanel* find(const char* panel_id) {
        return const_cast<CUiPanel*>(
            static_cast<const UiService*>(this)->find(panel_id));
    }

    const CUiPanel* find(const char* panel_id) const {
        if (panel_id == nullptr || *panel_id == '\0') return nullptr;
        auto view = app_->world_.registry().view<CUiPanel>();
        for (auto entity : view) {
            const auto& panel = view.get<CUiPanel>(entity);
            if (panel.id == panel_id) return &panel;
        }
        return nullptr;
    }

    Application* app_;
    /** Backs the ``const char*`` fields handed out by ``poll_click``: they must
     *  outlive the call, and the event they came from has been dequeued. */
    UiEvent event_scratch_{};
};

class Application::TransitionService final : public ISceneTransition {
public:
    explicit TransitionService(Application* app) : app_(app) {}

    bool go_to(const char* scene_id, const char* spawn_point_id) override {
        if (scene_id == nullptr || *scene_id == '\0') return false;
        return app_->request_scene_transition(
            scene_id, spawn_point_id == nullptr ? "" : spawn_point_id);
    }

    const char* current_scene() const override {
        return app_->scene_.scene_id.c_str();
    }

    bool scene_exists(const char* scene_id) const override {
        if (scene_id == nullptr) return false;
        return app_->scene_.scene_index.count(scene_id) != 0;
    }

    bool transition_pending() const override {
        return app_->pending_transition_.active;
    }

    bool restart_current(bool fresh) override {
        return app_->request_scene_restart(fresh);
    }

    bool load_nested(const char* scene_id, std::uint32_t host,
                     const char* anchor, float fit_extent_m) override {
        if (scene_id == nullptr) return false;
        return app_->load_nested_scene(scene_id, host,
                                       anchor == nullptr ? "" : anchor,
                                       fit_extent_m);
    }

    bool unload_nested(const char* scene_id) override {
        return scene_id != nullptr && app_->unload_nested_scene(scene_id);
    }

    bool nested_loaded(const char* scene_id) const override {
        return scene_id != nullptr && app_->nested_scene_loaded(scene_id);
    }

private:
    Application* app_;
};

class Application::PersistenceService final : public IPersistence {
public:
    explicit PersistenceService(Application* app) : app_(app) { (void)app_; }

    void set_flag(const char* key, bool value) override {
        if (key != nullptr) flags_[key] = value;
    }
    bool flag(const char* key, bool fallback) const override {
        if (key == nullptr) return fallback;
        const auto it = flags_.find(key);
        return it == flags_.end() ? fallback : it->second;
    }
    void set_number(const char* key, float value) override {
        if (key != nullptr) numbers_[key] = value;
    }
    float number(const char* key, float fallback) const override {
        if (key == nullptr) return fallback;
        const auto it = numbers_.find(key);
        return it == numbers_.end() ? fallback : it->second;
    }
    void set_text(const char* key, const char* value) override {
        if (key != nullptr) texts_[key] = value == nullptr ? "" : value;
    }
    const char* text(const char* key, const char* fallback) const override {
        if (key == nullptr) return fallback;
        const auto it = texts_.find(key);
        return it == texts_.end() ? fallback : it->second.c_str();
    }

    void set_scene_flag(const char* scene, const char* key,
                        bool value) override {
        set_flag(scoped(scene, key).c_str(), value);
    }
    bool scene_flag(const char* scene, const char* key,
                    bool fallback) const override {
        return flag(scoped(scene, key).c_str(), fallback);
    }
    void set_scene_number(const char* scene, const char* key,
                          float value) override {
        set_number(scoped(scene, key).c_str(), value);
    }
    float scene_number(const char* scene, const char* key,
                       float fallback) const override {
        return number(scoped(scene, key).c_str(), fallback);
    }

    bool has(const char* key) const override {
        if (key == nullptr) return false;
        return flags_.count(key) != 0 || numbers_.count(key) != 0 ||
               texts_.count(key) != 0;
    }
    void clear(const char* key) override {
        if (key == nullptr) return;
        flags_.erase(key);
        numbers_.erase(key);
        texts_.erase(key);
    }

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"flags", flags_},
            {"numbers", numbers_},
            {"texts", texts_},
        };
    }

    void from_json(const nlohmann::json& j) {
        if (!j.is_object()) return;
        flags_.clear();
        numbers_.clear();
        texts_.clear();
        if (j.contains("flags") && j["flags"].is_object()) {
            flags_ = j["flags"].get<std::unordered_map<std::string, bool>>();
        }
        if (j.contains("numbers") && j["numbers"].is_object()) {
            numbers_ =
                j["numbers"].get<std::unordered_map<std::string, float>>();
        }
        if (j.contains("texts") && j["texts"].is_object()) {
            texts_ =
                j["texts"]
                    .get<std::unordered_map<std::string, std::string>>();
        }
    }

private:
    /** ``scene/key``. Keeps a temple's puzzle state from colliding with a
     *  laboratory's when both call a key "door_sealed". */
    static std::string scoped(const char* scene, const char* key) {
        const std::string s = scene == nullptr ? "" : scene;
        const std::string k = key == nullptr ? "" : key;
        return s + "/" + k;
    }

    Application* app_;
    std::unordered_map<std::string, bool> flags_;
    std::unordered_map<std::string, float> numbers_;
    std::unordered_map<std::string, std::string> texts_;
};

class Application::EquipService final : public IEquip {
public:
    explicit EquipService(Application* app) : app_(app) {}

    bool equip(std::uint32_t item, std::uint32_t wearer,
               const char* slot) override {
        World& w = app_->world_;
        if (slot == nullptr || !w.valid(item) || !w.valid(wearer)) return false;
        const EquipSlot* def = find_slot(wearer, slot);
        if (def == nullptr) {
            PTV_WARN("equip: '{}' is not a slot on entity {}", slot, wearer);
            return false;
        }
        if (!accepts(*def, item)) {
            PTV_WARN("equip: entity {} is not accepted in slot '{}'", item,
                     slot);
            return false;
        }
        // A per-instance refusal, which is how a contaminated glove stays
        // out of a sterile slot even though its TYPE still fits.
        const auto ie = static_cast<entt::entity>(item);
        if (w.registry().all_of<CEquipped>(ie) &&
            !w.registry().get<CEquipped>(ie).re_equippable) {
            return false;
        }

        // Exclusive slots replace rather than stack. What happens to the
        // displaced item is the caller's policy: this service only guarantees
        // the slot ends up holding one thing.
        if (def->exclusive) {
            const std::uint32_t incumbent = equipped_at(wearer, slot);
            if (incumbent != InputSnapshot::kInvalidEntity &&
                incumbent != item) {
                unequip_internal(incumbent, /*force=*/true);
            }
        }

        // The slot's anchor is either a body slot on the wearer or a socket
        // on another equipped item. In the nested case the parent is that
        // item, so CAttachment chains and the geometry follows both hops.
        std::uint32_t parent = wearer;
        std::string anchor = def->body_slot;
        if (!def->host_item_slot.empty()) {
            const std::uint32_t host =
                equipped_at(wearer, def->host_item_slot.c_str());
            if (host == InputSnapshot::kInvalidEntity) {
                PTV_WARN("equip: slot '{}' needs '{}' equipped first", slot,
                         def->host_item_slot);
                return false;
            }
            parent = host;
            anchor = def->host_socket;
        }
        if (anchor.empty()) anchor = slot;

        if (app_->attach_svc_ == nullptr) return false;
        if (!app_->attach_svc_->attach(item, parent, anchor.c_str(),
                                       def->local_position,
                                       def->local_rotation)) {
            return false;
        }

        auto& eq = w.registry().get_or_emplace<CEquipped>(ie);
        eq.wearer = wearer;
        eq.slot = slot;
        eq.offset = Vec3(0.0f);
        eq.offset_rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        // Tag it so grab can recognise an equipped object without needing to
        // know anything about equipping.
        auto& tag = w.registry().get_or_emplace<CTag>(ie);
        if (std::find(tag.tags.begin(), tag.tags.end(), "equipped") ==
            tag.tags.end()) {
            tag.tags.push_back("equipped");
        }
        return true;
    }

    bool unequip(std::uint32_t item) override {
        return unequip_internal(item, /*force=*/false);
    }

    const char* slot_of(std::uint32_t item) const override {
        const World& w = app_->world_;
        const auto ie = static_cast<entt::entity>(item);
        if (!w.valid(item) || !w.registry().all_of<CEquipped>(ie)) return "";
        return w.registry().get<CEquipped>(ie).slot.c_str();
    }

    std::uint32_t equipped_at(std::uint32_t wearer,
                              const char* slot) const override {
        if (slot == nullptr) return InputSnapshot::kInvalidEntity;
        const World& w = app_->world_;
        auto view = w.registry().view<const CEquipped>();
        for (auto e : view) {
            const auto& eq = view.get<const CEquipped>(e);
            if (eq.wearer == wearer && eq.slot == slot) {
                return static_cast<std::uint32_t>(e);
            }
        }
        return InputSnapshot::kInvalidEntity;
    }

    int equipped_items(std::uint32_t wearer, std::uint32_t* out,
                       int max_out) const override {
        if (out == nullptr || max_out <= 0) return 0;
        int n = 0;
        auto view = app_->world_.registry().view<const CEquipped>();
        for (auto e : view) {
            if (n >= max_out) break;
            if (view.get<const CEquipped>(e).wearer != wearer) continue;
            out[n++] = static_cast<std::uint32_t>(e);
        }
        return n;
    }

    bool slot_exists(std::uint32_t wearer, const char* slot) const override {
        return slot != nullptr && find_slot(wearer, slot) != nullptr;
    }

    void set_removable(std::uint32_t item, bool removable) override {
        if (auto* eq = equipped_state(item)) eq->removable = removable;
    }

    void set_equippable(std::uint32_t item, bool equippable) override {
        World& w = app_->world_;
        if (!w.valid(item)) return;
        // Reachable on an item that is NOT currently equipped, which is the
        // point: a glove is marked contaminated after being taken off.
        auto& eq = w.registry().get_or_emplace<CEquipped>(
            static_cast<entt::entity>(item));
        eq.re_equippable = equippable;
    }

    bool is_removable(std::uint32_t item) const override {
        const World& w = app_->world_;
        const auto ie = static_cast<entt::entity>(item);
        if (!w.valid(item) || !w.registry().all_of<CEquipped>(ie)) return false;
        const auto& eq = w.registry().get<CEquipped>(ie);
        return eq.wearer != InputSnapshot::kInvalidEntity && eq.removable;
    }

    void set_offset(std::uint32_t item, const Vec3& position,
                    const Quat& rotation) override {
        auto* eq = equipped_state(item);
        if (eq == nullptr) return;
        eq->offset = position;
        eq->offset_rotation = glm::normalize(rotation);
        // Push it straight onto the attachment so the change is visible this
        // frame without waiting for a re-equip.
        World& w = app_->world_;
        const auto ie = static_cast<entt::entity>(item);
        if (!w.registry().all_of<CAttachment>(ie)) return;
        const EquipSlot* def = find_slot(eq->wearer, eq->slot.c_str());
        auto& att = w.registry().get<CAttachment>(ie);
        const Vec3 base = def ? def->local_position : Vec3(0.0f);
        const Quat base_rot = def ? def->local_rotation
                                  : Quat(1.0f, 0.0f, 0.0f, 0.0f);
        att.local_position = base + position;
        att.local_rotation = glm::normalize(base_rot * eq->offset_rotation);
    }

private:
    bool unequip_internal(std::uint32_t item, bool force) {
        World& w = app_->world_;
        const auto ie = static_cast<entt::entity>(item);
        if (!w.valid(item) || !w.registry().all_of<CEquipped>(ie)) return false;
        auto& eq = w.registry().get<CEquipped>(ie);
        if (eq.wearer == InputSnapshot::kInvalidEntity) return false;
        // ``removable`` gates deliberate removal only. ``force`` is for the
        // engine's own replacement path, which must not be blocked by a
        // gameplay rule about the player's hands.
        if (!force && !eq.removable) return false;

        if (app_->attach_svc_ != nullptr) app_->attach_svc_->detach(item);

        eq.wearer = InputSnapshot::kInvalidEntity;
        eq.slot.clear();
        if (w.registry().all_of<CTag>(ie)) {
            auto& tags = w.registry().get<CTag>(ie).tags;
            tags.erase(std::remove(tags.begin(), tags.end(), "equipped"),
                       tags.end());
        }
        return true;
    }

    CEquipped* equipped_state(std::uint32_t item) {
        World& w = app_->world_;
        const auto ie = static_cast<entt::entity>(item);
        if (!w.valid(item) || !w.registry().all_of<CEquipped>(ie)) {
            return nullptr;
        }
        return &w.registry().get<CEquipped>(ie);
    }

    const EquipSlot* find_slot(std::uint32_t wearer, const char* slot) const {
        const World& w = app_->world_;
        const auto we = static_cast<entt::entity>(wearer);
        if (!w.valid(wearer) || !w.registry().all_of<CEquipSlots>(we)) {
            return nullptr;
        }
        for (const auto& s : w.registry().get<CEquipSlots>(we).slots) {
            if (s.name == slot) return &s;
        }
        return nullptr;
    }

    /** Tag gate. An empty accept list means "anything", which keeps the
     *  freedom-first default: an object declared equippable with no further
     *  configuration goes anywhere on the body. */
    bool accepts(const EquipSlot& def, std::uint32_t item) const {
        if (def.accepts_tags.empty()) return true;
        const World& w = app_->world_;
        const auto ie = static_cast<entt::entity>(item);
        if (!w.registry().all_of<CTag>(ie)) return false;
        const auto& tags = w.registry().get<CTag>(ie).tags;
        for (const auto& want : def.accepts_tags) {
            if (std::find(tags.begin(), tags.end(), want) != tags.end()) {
                return true;
            }
        }
        return false;
    }

    Application* app_;
};

class Application::CombatService final : public ICombat {
public:
    explicit CombatService(Application* app) : app_(app) {}

    bool apply_damage(std::uint32_t target, float amount,
                      std::uint32_t instigator) override {
        return apply_impl(target, amount, instigator, false);
    }

    bool apply_weapon_damage(std::uint32_t target, float amount,
                             std::uint32_t instigator) override {
        const bool on_player = target == static_cast<std::uint32_t>(app_->player_entity_);
        return apply_impl(target, amount, instigator, !on_player);
    }

    void set_blocker(std::uint32_t shield, bool enabled, float leak) override {
        if (enabled) {
            blockers_[shield] = std::clamp(leak, 0.0f, 1.0f);
        } else {
            blockers_.erase(shield);
        }
    }

    std::uint64_t blocked_seq(std::uint32_t shield) const override {
        const auto it = blocked_.find(shield);
        return it == blocked_.end() ? 0 : it->second;
    }

    /** A registered shield held by ``target`` that the attacker's line
     *  to the target passes through, or invalid. */
    std::uint32_t blocking_shield(std::uint32_t target, std::uint32_t instigator) const {
        World& w = app_->world_;
        if (blockers_.empty() || !w.valid(instigator) || !w.has<CTransform>(instigator) ||
            !w.has<CTransform>(target)) {
            return InputSnapshot::kInvalidEntity;
        }
        for (const auto& [shield, leak] : blockers_) {
            (void)leak;
            bool held_by_target = false;
            if (target == static_cast<std::uint32_t>(app_->player_entity_)) {
                for (const auto& hand : app_->hands_) {
                    if (hand.held == static_cast<entt::entity>(shield)) held_by_target = true;
                }
            } else if (w.valid(shield) && w.has<CAttachment>(shield) &&
                       w.get<CAttachment>(shield).parent == target) {
                held_by_target = true;
            }
            if (!held_by_target) continue;
            const Vec3 from = w.get<CTransform>(instigator).t.position + Vec3(0.0f, 1.1f, 0.0f);
            Vec3 to = w.get<CTransform>(target).t.position;
            if (target == static_cast<std::uint32_t>(app_->player_entity_)) {
                to = app_->last_head_world_pos_ - Vec3(0.0f, 0.35f, 0.0f);
            }
            const Vec3 d = to - from;
            const float len = glm::length(d);
            if (len < 1e-3f) continue;
            PhysicsWorld::RayFilter f{};
            f.ignore.push_back(instigator);
            Vec3 hit(0.0f), n(0.0f);
            std::uint32_t e = InputSnapshot::kInvalidEntity;
            if (app_->physics_.raycast(from, d / len, len + 0.3f, f, hit, n, e) && e == shield) {
                return shield;
            }
        }
        return InputSnapshot::kInvalidEntity;
    }

    bool apply_impl(std::uint32_t target, float amount, std::uint32_t instigator,
                    bool ignore_invuln) {
        World& w = app_->world_;
        if (!w.valid(target) || !w.has<CHealth>(target)) return false;
        if (!std::isfinite(amount) || amount <= 0.0f) return false;

        auto& hp = w.get<CHealth>(target);
        if (!hp.alive || hp.death_timer_s >= 0.0f) return false;
        if (hp.invuln_s > 0.0f && !ignore_invuln) return false;
        // A shield in the way takes the blow.
        const std::uint32_t shield = blocking_shield(target, instigator);
        if (shield != InputSnapshot::kInvalidEntity) {
            ++blocked_[shield];
            amount *= blockers_[shield];
            if (amount <= 0.0f) return false;
        }

        // Verification keeps the player alive so a test run can watch the
        // whole encounter; the hit is still recorded below.
        const bool protect = app_->verify_mode_ && !app_->verify_flow_ &&
                             target == static_cast<std::uint32_t>(app_->player_entity_);
        if (!protect) hp.current = std::max(0.0f, hp.current - amount);
        hp.invuln_s = std::max(0.0f, hp.invuln_after_hit_s);
        // A protected hit is published with no damage, so generated health
        // that mirrors the feed stays full too -- otherwise it would end the
        // run the engine is keeping alive. It is still logged and counted.
        record(w, target, instigator, protect ? 0.0f : amount, amount,
               hp.current, hp.current <= 0.0f);

        if (hp.current <= 0.0f) {
            app_->begin_death(target);
        } else if (app_->mech_ctx_.skeleton != nullptr) {
            // A hit reaction is a one-shot and reads better as an instant
            // cut than a blend -- the point is that it is abrupt. Reached
            // through the interface rather than the concrete service because
            // SkeletonService is declared further down this file.
            app_->mech_ctx_.skeleton->play_clip(target, "hurt",
                                                /*looping=*/false, 0.0f);
        }
        return true;
    }

    void heal(std::uint32_t entity, float amount) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CHealth>(entity)) return;
        if (!std::isfinite(amount) || amount <= 0.0f) return;
        auto& hp = w.get<CHealth>(entity);
        if (!hp.alive) return;
        hp.current = std::min(hp.max, hp.current + amount);
    }

    float health(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CHealth>(entity)) return 0.0f;
        return w.get<CHealth>(entity).current;
    }

    float max_health(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CHealth>(entity)) return 0.0f;
        return w.get<CHealth>(entity).max;
    }

    bool is_alive(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CHealth>(entity)) return false;
        const auto& hp = w.get<CHealth>(entity);
        return hp.alive && hp.death_timer_s < 0.0f;
    }

    void kill(std::uint32_t entity) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CHealth>(entity)) return;
        auto& hp = w.get<CHealth>(entity);
        if (!hp.alive || hp.death_timer_s >= 0.0f) return;
        hp.current = 0.0f;
        app_->begin_death(entity);
    }

    bool revive(std::uint32_t entity, float health,
                float invulnerable_s) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CHealth>(entity)) return false;
        auto& hp = w.get<CHealth>(entity);
        hp.alive = true;
        hp.death_timer_s = -1.0f;
        hp.current = (std::isfinite(health) && health > 0.0f)
                         ? std::min(health, hp.max)
                         : hp.max;
        if (std::isfinite(invulnerable_s)) {
            hp.invuln_s = std::max(hp.invuln_s, std::clamp(invulnerable_s, 0.0f, 30.0f));
        }
        // An NPC's body was removed when it died; the player's never is.
        if (entity != static_cast<std::uint32_t>(app_->player_entity_)) {
            app_->physics_.add_body_for(app_->world_, entity);
        }
        if (app_->mech_ctx_.skeleton != nullptr) {
            app_->mech_ctx_.skeleton->stop_clip(entity);
        }
        PTV_INFO("combat: {} revived with {:.1f} health", entity, hp.current);
        return true;
    }

    const char* faction(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CFaction>(entity)) return "";
        return w.get<CFaction>(entity).id.c_str();
    }

    bool is_hostile(std::uint32_t a, std::uint32_t b) const override {
        if (a == b) return false;
        const World& w = app_->world_;
        if (!w.valid(a) || !w.valid(b)) return false;
        if (!w.has<CFaction>(a) || !w.has<CFaction>(b)) return false;
        return factions_hostile(w.get<CFaction>(a), w.get<CFaction>(b));
    }

    std::uint64_t damage_seq(std::uint32_t entity) const override {
        return log_.last_seq(entity);
    }

    int damage_events_since(std::uint32_t entity, std::uint64_t after_seq,
                            DamageEvent* out, int max_out) const override {
        return log_.since(entity, after_seq, out, max_out);
    }

    /** Hits that landed on the player since the scene loaded. */
    int player_hits() const { return player_hits_; }

    void clear() {
        log_.clear();
        player_hits_ = 0;
    }

private:
    void record(const World& w, std::uint32_t target, std::uint32_t instigator,
                float amount, float dealt, float health_after, bool killed) {
        DamageEvent ev{};
        ev.target = target;
        ev.instigator = instigator;
        ev.amount = amount;
        ev.health_after = health_after;
        ev.killed = killed;
        const std::uint32_t src =
            (instigator != InputSnapshot::kInvalidEntity && w.valid(instigator) &&
             w.has<CTransform>(instigator))
                ? instigator
                : target;
        if (w.has<CTransform>(src)) ev.source_position = w.get<CTransform>(src).t.position;
        log_.record(ev);
        const bool on_player =
            target == static_cast<std::uint32_t>(app_->player_entity_);
        if (on_player) ++player_hits_;
        // Not in the weapons pass: there the target has to survive long
        // enough to be shot.
        if (on_player && app_->verify_mode_ && !app_->verify_flow_ &&
            !app_->verify_weapons_ && instigator != InputSnapshot::kInvalidEntity) {
            auto& q = app_->verify_defeats_;
            const bool queued = std::any_of(q.begin(), q.end(), [&](const auto& p) {
                return p.first == instigator;
            });
            // Long enough for the attack clip to read in a capture.
            if (!queued) q.emplace_back(instigator, 0.8f);
        }
        // Hits are rare enough to log every one, and the on-device
        // verification counts them.
        PTV_INFO("combat: {} hit {}{} for {:.1f} (health {:.1f}{})", instigator,
                 target, on_player ? " (player)" : "", dealt, health_after,
                 killed ? ", killed" : "");
    }

    Application* app_;
    DamageLog log_{};
    int player_hits_{0};
    std::unordered_map<std::uint32_t, float> blockers_;
    std::unordered_map<std::uint32_t, std::uint64_t> blocked_;
};

class Application::PerceptionService final : public IPerception {
public:
    explicit PerceptionService(Application* app) : app_(app) {}

    std::uint32_t nearest_hostile(std::uint32_t self,
                                  float max_range) const override {
        std::uint32_t best = InputSnapshot::kInvalidEntity;
        float best_d2 = 0.0f;
        each_hostile(self, max_range, [&](std::uint32_t other, float d2) {
            if (best == InputSnapshot::kInvalidEntity || d2 < best_d2) {
                best = other;
                best_d2 = d2;
            }
        });
        return best;
    }

    int hostiles_in_range(std::uint32_t self, float max_range,
                          std::uint32_t* out, int max_out) const override {
        if (out == nullptr || max_out <= 0) return 0;
        std::vector<std::pair<float, std::uint32_t>> found;
        each_hostile(self, max_range, [&](std::uint32_t other, float d2) {
            found.emplace_back(d2, other);
        });
        std::sort(found.begin(), found.end());
        const int n = std::min<int>(max_out, static_cast<int>(found.size()));
        for (int i = 0; i < n; ++i) out[i] = found[static_cast<size_t>(i)].second;
        return n;
    }

    bool can_see(std::uint32_t self, std::uint32_t target, float fov_deg,
                 float max_range) const override {
        const World& w = app_->world_;
        if (!w.valid(self) || !w.valid(target)) return false;
        if (!w.has<CTransform>(self) || !w.has<CTransform>(target)) {
            return false;
        }
        const Vec3 from = eye_of(self);
        const Vec3 to = eye_of(target);
        Vec3 d = to - from;
        const float dist = glm::length(d);
        if (dist > std::max(0.0f, max_range)) return false;
        if (dist < 1.0e-4f) return true;
        d /= dist;

        // Field of view is measured horizontally: a character that has to
        // crane its neck to see something still counts, but one facing away
        // does not.
        const Quat rot = w.get<CTransform>(self).t.rotation;
        Vec3 forward = rot * Vec3(0.0f, 0.0f, -1.0f);
        Vec3 flat_f(forward.x, 0.0f, forward.z);
        Vec3 flat_d(d.x, 0.0f, d.z);
        if (glm::length(flat_f) > 1.0e-4f && glm::length(flat_d) > 1.0e-4f) {
            const float cosang =
                glm::dot(glm::normalize(flat_f), glm::normalize(flat_d));
            const float half = glm::radians(std::clamp(fov_deg, 1.0f, 360.0f)) *
                               0.5f;
            if (cosang < std::cos(half)) return false;
        }
        return unobstructed(self, target, from, to);
    }

    bool line_of_sight(std::uint32_t from, std::uint32_t to) const override {
        const World& w = app_->world_;
        if (!w.valid(from) || !w.valid(to)) return false;
        if (!w.has<CTransform>(from) || !w.has<CTransform>(to)) return false;
        return unobstructed(from, to, eye_of(from), eye_of(to));
    }

private:
    /** Roughly head height, so sight lines clear waist-high cover the way a
     *  player's would. */
    Vec3 eye_of(std::uint32_t entity) const {
        const World& w = app_->world_;
        Vec3 p = w.get<CTransform>(entity).t.position;
        float height = w.get<CTransform>(entity).t.scale;
        if (w.has<CMesh>(entity)) {
            const auto& m = w.get<CMesh>(entity);
            if (m.target_scale_meters > 1.0e-4f) height = m.target_scale_meters;
        }
        p.y += 0.35f * std::max(0.2f, height);
        return p;
    }

    /** Nothing solid between the two, ignoring the endpoints themselves. */
    bool unobstructed(std::uint32_t self, std::uint32_t target,
                      const Vec3& from, const Vec3& to) const {
        Vec3 d = to - from;
        const float dist = glm::length(d);
        if (dist < 1.0e-4f) return true;
        d /= dist;

        Vec3 hit(0.0f);
        Vec3 normal(0.0f, 1.0f, 0.0f);
        std::uint32_t blocker = InputSnapshot::kInvalidEntity;
        // Both endpoints' own bodies are passed through. The eye sits inside
        // the looker's capsule, so without that the first hit was always the
        // looker itself -- which then counted as "clear" and let every NPC
        // see (and hit) through walls.
        PhysicsWorld::RayFilter filter{};
        filter.ignore = {self, target};
        if (!app_->physics_.raycast(from, d, dist, filter, hit, normal,
                                    blocker)) {
            return true;
        }
        return blocker == InputSnapshot::kInvalidEntity;
    }

    template <typename Fn>
    void each_hostile(std::uint32_t self, float max_range, Fn&& fn) const {
        const World& w = app_->world_;
        if (!w.valid(self) || !w.has<CTransform>(self)) return;
        if (!w.has<CFaction>(self)) return;
        const Vec3 origin = w.get<CTransform>(self).t.position;
        const float range = std::max(0.0f, max_range);
        const float range2 = range * range;

        auto view = app_->world_.registry().view<CFaction, CTransform>();
        for (auto other : view) {
            const auto id = static_cast<std::uint32_t>(other);
            if (id == self) continue;
            if (app_->combat_svc_ && !app_->combat_svc_->is_alive(id)) continue;
            if (app_->combat_svc_ == nullptr ||
                !app_->combat_svc_->is_hostile(self, id)) {
                continue;
            }
            const Vec3 d = view.get<CTransform>(other).t.position - origin;
            const float d2 = glm::dot(d, d);
            if (d2 > range2) continue;
            fn(id, d2);
        }
    }

    Application* app_;
};

class Application::NavigationService final : public INavigation {
public:
    explicit NavigationService(Application* app) : app_(app) {}

    Vec3 next_waypoint(std::uint32_t agent, const Vec3& target) override {
        World& w = app_->world_;
        if (!w.valid(agent) || !w.has<CTransform>(agent)) return target;
        const Vec3 pos = w.get<CTransform>(agent).t.position;

        // --- Global leg -----------------------------------------------
        //
        // Steering alone cannot find a doorway: it only knows what its
        // whiskers hit this frame, so it presses against the wall beside the
        // opening. Routing supplies the next corner; steering then handles
        // getting there.
        Vec3 goal = target;
        if (!app_->nav_graph_.empty()) {
            goal = corridor_goal(agent, pos, target);
        }

        Vec3 desired(goal.x - pos.x, 0.0f, goal.z - pos.z);
        const float dist = glm::length(desired);
        if (dist < 1.0e-4f) return goal;
        desired /= dist;

        // --- Local steering -------------------------------------------
        const Vec3 steer = avoid(agent, pos, desired);
        const float step = std::min(dist, kLookaheadMeters);
        return project_to_ground(pos + steer * step);
    }

    bool is_reachable(const Vec3& from, const Vec3& to) const override {
        return app_->nav_graph_.is_reachable(from, to);
    }

    bool path_distance(const Vec3& from, const Vec3& to,
                       float& out_meters) const override {
        if (app_->nav_graph_.empty()) {
            // Without declared topology the honest answer is the straight
            // line, which is what a caller comparing two targets needs.
            const Vec3 d(to.x - from.x, 0.0f, to.z - from.z);
            out_meters = glm::length(d);
            return true;
        }
        return app_->nav_graph_.path_distance(from, to, out_meters);
    }

    Vec3 project_to_ground(const Vec3& position) const override {
        PhysicsRayHit hit{};
        const Vec3 from(position.x, position.y + kGroundProbeUp,
                        position.z);
        if (!app_->physics_svc_) return position;
        // The floor, not a character standing on it (or the one asking).
        if (!app_->physics_svc_->raycast_floor(
                InputSnapshot::kInvalidEntity, from, Vec3(0.0f, -1.0f, 0.0f),
                kGroundProbeUp + kGroundProbeDown, hit)) {
            return position;
        }
        return Vec3(position.x, hit.point.y, position.z);
    }

    Vec3 nearest_walkable(const Vec3& position, float margin_m) const override {
        const Vec3 p = app_->nav_graph_.nearest_inside(position, margin_m);
        return p == position ? position : project_to_ground(Vec3(p.x, position.y, p.z));
    }

    bool line_of_sight(const Vec3& from, const Vec3& to) const override {
        if (!app_->physics_svc_) return true;
        Vec3 d = to - from;
        const float dist = glm::length(d);
        if (dist < 1.0e-4f) return true;
        d /= dist;
        PhysicsRayHit hit{};
        // Characters do not block sight here: the usual caller is a point at
        // the player's eye, which sits inside the player's own capsule.
        return !app_->physics_svc_->raycast_excluding(
            InputSnapshot::kInvalidEntity, from, d, dist, hit,
            /*floor_only=*/true);
    }

private:
    // How far ahead to aim. Short enough that the returned point is always
    // somewhere the agent can actually reach this second, long enough that
    // the direction is stable frame to frame.
    static constexpr float kLookaheadMeters = 1.5f;
    static constexpr float kWhiskerMeters = 1.6f;
    static constexpr float kGroundProbeUp = 1.5f;
    static constexpr float kGroundProbeDown = 4.0f;
    static constexpr float kSeparationRadius = 1.1f;
    static constexpr float kCornerReachedMeters = 0.6f;

    /** The next corner of the routed corridor, cached per agent.
     *
     *  Recomputed only when the destination moves appreciably or the corner
     *  is reached. A* every frame for every NPC is exactly the cost that
     *  makes navigation unaffordable on a Quest, and the answer barely
     *  changes between frames. */
    Vec3 corridor_goal(std::uint32_t agent, const Vec3& pos,
                       const Vec3& target) {
        auto& state = paths_[agent];
        const Vec3 flat_target(target.x, 0.0f, target.z);
        const bool target_moved =
            glm::length(flat_target - state.target_xz) > kRepathTargetMeters;
        const bool corner_reached =
            state.has_corner &&
            glm::length(Vec3(state.corner.x - pos.x, 0.0f,
                             state.corner.z - pos.z)) < kCornerReachedMeters;

        if (target_moved || corner_reached || !state.has_corner) {
            std::vector<Vec3> corners;
            if (app_->nav_graph_.find_path(pos, target, corners) &&
                corners.size() >= 2) {
                state.corner = corners[1];
                state.has_corner = true;
            } else {
                // No route: head straight at it. Better to walk up to the
                // nearest wall than to stand still, and the caller can ask
                // is_reachable when it needs certainty.
                state.corner = target;
                state.has_corner = true;
            }
            state.target_xz = flat_target;
        }
        return state.corner;
    }

    /** Steer ``desired`` around walls and other agents. */
    Vec3 avoid(std::uint32_t agent, const Vec3& pos, const Vec3& desired) {
        Vec3 out = desired;

        // Whiskers. Cast at chest height so a kerb does not read as a wall.
        if (app_->physics_svc_ != nullptr) {
            const Vec3 eye(pos.x, pos.y + 0.4f, pos.z);
            const Vec3 left = rotate_y(desired, glm::radians(35.0f));
            const Vec3 right = rotate_y(desired, glm::radians(-35.0f));

            const float ahead = clearance(agent, eye, desired);
            if (ahead < kWhiskerMeters) {
                const float cl = clearance(agent, eye, left);
                const float cr = clearance(agent, eye, right);
                // Turn toward the side with more room. Blending rather than
                // replacing keeps the agent making progress along its
                // original heading instead of orbiting the obstacle.
                const Vec3 side = (cl >= cr) ? left : right;
                const float blend =
                    1.0f - std::clamp(ahead / kWhiskerMeters, 0.0f, 1.0f);
                out = glm::mix(out, side, blend);
            }
        }

        // Separation. Without it ten enemies converging on the player stack
        // into one another and read as a single flickering mass.
        Vec3 push(0.0f);
        auto view = app_->world_.registry().view<CLocomotionIntent, CTransform>();
        for (auto other : view) {
            const auto id = static_cast<std::uint32_t>(other);
            if (id == agent) continue;
            Vec3 d = pos - view.get<CTransform>(other).t.position;
            d.y = 0.0f;
            const float dist = glm::length(d);
            if (dist > kSeparationRadius || dist < 1.0e-4f) continue;
            push += (d / dist) * (1.0f - dist / kSeparationRadius);
        }
        if (glm::length(push) > 1.0e-4f) {
            out += glm::normalize(push) * kSeparationWeight;
        }

        const float len = glm::length(out);
        return (len > 1.0e-4f) ? (out / len) : desired;
    }

    /** Free distance along ``dir``. The whisker starts inside the agent's own
     *  capsule, so that body -- and other characters, which separation
     *  handles -- are passed through; otherwise every whisker read zero and
     *  every agent veered left forever. */
    float clearance(std::uint32_t agent, const Vec3& from,
                    const Vec3& dir) const {
        PhysicsRayHit hit{};
        if (!app_->physics_svc_->raycast_excluding(agent, from, dir,
                                                   kWhiskerMeters, hit,
                                                   /*floor_only=*/true)) {
            return kWhiskerMeters;
        }
        return glm::length(hit.point - from);
    }

    static Vec3 rotate_y(const Vec3& v, float radians) {
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        return Vec3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c);
    }

    static constexpr float kRepathTargetMeters = 1.0f;
    static constexpr float kSeparationWeight = 0.8f;

    struct PathState {
        Vec3 target_xz{0.0f};
        Vec3 corner{0.0f};
        bool has_corner{false};
    };

    Application* app_;
    std::unordered_map<std::uint32_t, PathState> paths_{};
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
                return base * skin_matrix_for(w, entity, name) *
                       it->second.matrix();
            }
        }
        return base * fallback_local(w, entity);
    }

private:
    /** The skinning matrix that carries this socket, or identity.
     *
     *  A socket is treated exactly like a vertex rigidly bound to one joint:
     *  ``palette[j]`` is what moves the surrounding geometry, so pushing the
     *  socket through the same matrix keeps it welded to the limb. In the
     *  bind pose every palette entry is the identity, so a static prop --
     *  and a rigged character standing in bind pose -- behaves exactly as it
     *  did before sockets learned about skeletons. */
    static Mat4 skin_matrix_for(const World& w, std::uint32_t entity,
                                const char* name) {
        if (!w.has<CSkeleton>(entity)) return Mat4(1.0f);
        const auto& sk = w.get<CSkeleton>(entity);
        const auto it = sk.socket_joint.find(name);
        if (it == sk.socket_joint.end() || it->second < 0) return Mat4(1.0f);
        const auto j = static_cast<size_t>(it->second);
        if (j >= sk.palette.size()) return Mat4(1.0f);
        return sk.palette[j];
    }

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

class Application::SkeletonService final : public ISkeleton {
public:
    explicit SkeletonService(Application* app) : app_(app) {}

    bool has_skeleton(std::uint32_t entity) const override {
        const World& w = app_->world_;
        return w.valid(entity) && w.has<CSkeleton>(entity) &&
               w.get<CSkeleton>(entity).skeleton.joint_count() > 0;
    }

    int joint_index(std::uint32_t entity, const char* name) const override {
        if (name == nullptr || !has_skeleton(entity)) return -1;
        const auto& sk = app_->world_.get<CSkeleton>(entity).skeleton;
        const auto it = sk.name_to_joint.find(name);
        return (it == sk.name_to_joint.end()) ? -1 : it->second;
    }

    int joint_count(std::uint32_t entity) const override {
        if (!has_skeleton(entity)) return 0;
        return static_cast<int>(
            app_->world_.get<CSkeleton>(entity).skeleton.joint_count());
    }

    int joint_for_slot(std::uint32_t entity, const char* slot) const override {
        if (slot == nullptr || !has_skeleton(entity)) return -1;
        const auto& candidates = body_slot_candidates(slot);
        if (candidates.empty()) return -1;
        const auto& sk = app_->world_.get<CSkeleton>(entity).skeleton;

        // Candidates are stored normalized, so the rig's names have to be
        // normalized too. Build that map once per skeleton rather than per
        // lookup: a full-body rig is ~65 joints and equip resolves several
        // slots at attach time.
        const auto& lookup = normalized_names_for(sk);
        for (const auto& candidate : candidates) {
            const auto it = lookup.find(candidate);
            if (it != lookup.end()) return it->second;
        }
        return -1;
    }

    int joint_parent(std::uint32_t entity, int joint) const override {
        if (!has_skeleton(entity)) return -1;
        const auto& sk = app_->world_.get<CSkeleton>(entity).skeleton;
        if (joint < 0 || static_cast<size_t>(joint) >= sk.parents.size()) {
            return -1;
        }
        return sk.parents[static_cast<size_t>(joint)];
    }

    Mat4 joint_world(std::uint32_t entity, int joint) const override {
        const World& w = app_->world_;
        if (!valid_joint(entity, joint) || !w.has<CTransform>(entity)) {
            return Mat4(1.0f);
        }
        // Recomputed from local_pose rather than read back out of the
        // palette: the palette is only rebuilt by the pose system AFTER the
        // Simulate phase, so during Simulate it describes last frame. An IK
        // solver that wrote the upper arm and then read the forearm from the
        // palette saw the forearm where the previous frame left it, which is
        // the feedback loop that made avatar arms flail.
        const auto& skc = w.get<CSkeleton>(entity);
        // Scale-free like joint_model: a rig authored in centimetres scales
        // its root joint by 0.01, and an offset composed through that would
        // shrink a hundredfold.
        return w.get<CTransform>(entity).t.matrix() *
               orthonormal(fk_metres(skc, current_pose(skc), joint));
    }

    Mat4 joint_model(std::uint32_t entity, int joint) const override {
        if (!valid_joint(entity, joint)) return Mat4(1.0f);
        const auto& skc = app_->world_.get<CSkeleton>(entity);
        return orthonormal(fk_metres(skc, current_pose(skc), joint));
    }

    Mat4 joint_bind_model(std::uint32_t entity, int joint) const override {
        if (!valid_joint(entity, joint)) return Mat4(1.0f);
        const auto& skc = app_->world_.get<CSkeleton>(entity);
        return orthonormal(fk_metres(skc, skc.skeleton.bind_pose, joint));
    }

    Quat joint_bind_rotation(std::uint32_t entity, int joint) const override {
        if (!valid_joint(entity, joint)) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
        return app_->world_.get<CSkeleton>(entity)
            .skeleton.bind_pose[static_cast<size_t>(joint)]
            .rotation;
    }

    Quat joint_local_rotation(std::uint32_t entity, int joint) const override {
        if (!valid_joint(entity, joint)) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
        const auto& skc = app_->world_.get<CSkeleton>(entity);
        return current_pose(skc)[static_cast<size_t>(joint)].rotation;
    }

    void set_joint_rotation_from_bind(std::uint32_t entity, int joint,
                                      const Quat& delta) override {
        if (!valid_joint(entity, joint)) return;
        set_joint_rotation(entity, joint,
                           joint_bind_rotation(entity, joint) * delta);
    }

    int joint_subtree(std::uint32_t entity, int root, int* out,
                      int max_out) const override {
        if (out == nullptr || max_out <= 0 || !has_skeleton(entity)) return 0;
        const auto& sk = app_->world_.get<CSkeleton>(entity).skeleton;
        const int count = static_cast<int>(sk.parents.size());
        if (root < 0 || root >= count) return 0;

        int written = 0;
        out[written++] = root;
        // ``order`` is parent-first, so one forward pass is enough: a joint's
        // parent has already been accepted or rejected by the time it is
        // visited. No recursion, no second pass.
        for (const int j : sk.order) {
            if (j == root || j < 0 || j >= count) continue;
            const int parent = sk.parents[static_cast<size_t>(j)];
            if (parent < 0) continue;
            const bool parent_in =
                std::find(out, out + written, parent) != out + written;
            if (!parent_in) continue;
            if (written >= max_out) break;
            out[written++] = j;
        }
        return written;
    }

    void set_joint_rotation(std::uint32_t entity, int joint,
                            const Quat& rotation) override {
        CSkeleton* sk = nullptr;
        CAnimator* anim = nullptr;
        if (!resolve(entity, joint, &sk, &anim)) return;
        // Normalizing here rather than trusting the caller: generated code
        // routinely builds rotations by multiplying several quaternions, and
        // drift there shows up as a joint that slowly inflates the mesh.
        sk->local_pose[static_cast<size_t>(joint)].rotation =
            glm::normalize(rotation);
        anim->joint_overridden[static_cast<size_t>(joint)] = 1u;
    }

    void set_joint_translation(std::uint32_t entity, int joint,
                               const Vec3& translation) override {
        CSkeleton* sk = nullptr;
        CAnimator* anim = nullptr;
        if (!resolve(entity, joint, &sk, &anim)) return;
        sk->local_pose[static_cast<size_t>(joint)].translation = translation;
        anim->joint_overridden[static_cast<size_t>(joint)] = 1u;
    }

    void set_joint_scale(std::uint32_t entity, int joint,
                         const Vec3& scale) override {
        CSkeleton* sk = nullptr;
        CAnimator* anim = nullptr;
        if (!resolve(entity, joint, &sk, &anim)) return;
        // Negative scale would mirror the subtree and invert its winding,
        // which reads as inside-out geometry rather than as an error.
        sk->local_pose[static_cast<size_t>(joint)].scale =
            glm::max(scale, Vec3(0.0f));
        anim->joint_overridden[static_cast<size_t>(joint)] = 1u;
    }

    bool play_clip(std::uint32_t entity, const char* clip_name, bool looping,
                   float fade_s) override {
        if (clip_name == nullptr || !has_skeleton(entity)) return false;
        World& w = app_->world_;
        if (!w.has<CAnimator>(entity)) return false;

        const int index = clip_index(entity, clip_name);
        if (index < 0) return false;

        auto& anim = w.get<CAnimator>(entity);
        // Re-asking for the clip that is already running is the normal case
        // for a per-frame "keep walking", so it must NOT restart the cycle --
        // that would freeze the character on frame zero of its walk forever.
        if (anim.active_clip == index && anim.playing) {
            anim.looping = looping;
            return true;
        }

        const float fade = std::clamp(fade_s, 0.0f, 1.0f);
        if (fade > 0.0f && anim.active_clip >= 0 &&
            anim.active_clip != index) {
            anim.prev_clip = anim.active_clip;
            anim.prev_time_s = anim.time_s;
            anim.blend_t = fade;
            anim.blend_duration = fade;
        } else {
            anim.prev_clip = -1;
            anim.blend_t = 0.0f;
            anim.blend_duration = 0.0f;
        }

        anim.active_clip = index;
        anim.time_s = 0.0f;
        anim.looping = looping;
        anim.playing = true;
        return true;
    }

    void stop_clip(std::uint32_t entity) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return;
        w.get<CAnimator>(entity).playing = false;
    }

    void fade_to_rest(std::uint32_t entity, float fade_s) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return;
        auto& anim = w.get<CAnimator>(entity);
        if (anim.active_clip < 0) return;  // already at rest
        // The animator cross-fades toward the bind pose when there is no
        // incoming clip, so handing the active clip to the outgoing slot and
        // clearing it is a faded stop.
        const float fade = std::clamp(fade_s, 0.0f, 1.0f);
        if (fade > 0.0f) {
            anim.prev_clip = anim.active_clip;
            anim.prev_time_s = anim.time_s;
            anim.blend_t = fade;
            anim.blend_duration = fade;
        }
        anim.active_clip = -1;
        anim.time_s = 0.0f;
        anim.playing = false;
    }

    bool one_shot_playing(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return false;
        const auto& anim = w.get<CAnimator>(entity);
        return anim.active_clip >= 0 && !anim.looping && anim.playing;
    }

    void set_clip_speed(std::uint32_t entity, float speed) override {
        World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return;
        // Engine-clamped like every other service: a runaway speed from
        // generated code would otherwise alias the clip into noise.
        w.get<CAnimator>(entity).speed = std::clamp(speed, -8.0f, 8.0f);
    }

    bool has_clip(std::uint32_t entity, const char* clip_name) const override {
        return clip_index(entity, clip_name) >= 0;
    }

    const char* current_clip(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return "";
        const auto& anim = w.get<CAnimator>(entity);
        if (anim.active_clip < 0 ||
            static_cast<size_t>(anim.active_clip) >= anim.clips.size()) {
            return "";
        }
        const std::string& real = anim.clips[
            static_cast<size_t>(anim.active_clip)].name;
        // Report the INTENT, not Tripo's clip name: the caller asked for
        // "walk" and comparing against "Armature|preset_walk" would never
        // match anything they could have written.
        if (w.has<CMesh>(entity)) {
            for (const auto& [intent, name] : w.get<CMesh>(entity).animations) {
                if (name == real) return intent.c_str();
            }
        }
        return real.c_str();
    }

    float clip_time(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return 0.0f;
        return w.get<CAnimator>(entity).time_s;
    }

    float clip_duration(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return 0.0f;
        const auto& anim = w.get<CAnimator>(entity);
        if (anim.active_clip < 0 ||
            static_cast<size_t>(anim.active_clip) >= anim.clips.size()) {
            return 0.0f;
        }
        return anim.clips[static_cast<size_t>(anim.active_clip)].duration;
    }

    bool clip_finished(std::uint32_t entity) const override {
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return false;
        const auto& anim = w.get<CAnimator>(entity);
        if (anim.active_clip < 0) return false;
        // A looping clip never finishes, and saying otherwise would make a
        // caller waiting on a one-shot fire on the first frame.
        if (anim.looping) return false;
        return !anim.playing;
    }

private:
    bool valid_joint(std::uint32_t entity, int joint) const {
        if (!has_skeleton(entity)) return false;
        const auto& sk = app_->world_.get<CSkeleton>(entity).skeleton;
        return joint >= 0 && static_cast<size_t>(joint) < sk.joint_count();
    }

    /** The pose a joint query should read: the live local pose once the
     *  pose system has sized it, the bind pose before that. */
    static const std::vector<gltf_mesh_load::JointPose>& current_pose(
        const CSkeleton& skc) {
        return (skc.local_pose.size() == skc.skeleton.joint_count())
                   ? skc.local_pose
                   : skc.skeleton.bind_pose;
    }

    /** Forward kinematics from ``pose`` up to ``joint``, in metres.
     *
     *  Joint matrices live in the source file's units while the loader
     *  scaled the vertices by ``unit_scale``; conjugating the whole chain by
     *  that uniform scale reduces to scaling each local translation, which
     *  is what ``build_joint_palette`` does to the palette as well. */
    static Mat4 fk_metres(const CSkeleton& skc,
                          const std::vector<gltf_mesh_load::JointPose>& pose,
                          int joint) {
        const auto& sk = skc.skeleton;
        const float u = sk.unit_scale;
        Mat4 m(1.0f);
        int j = joint;
        size_t guard = 0;
        while (j >= 0 && static_cast<size_t>(j) < pose.size() &&
               guard++ <= sk.joint_count()) {
            gltf_mesh_load::JointPose p = pose[static_cast<size_t>(j)];
            p.translation *= u;
            m = p.matrix() * m;
            j = sk.parents[static_cast<size_t>(j)];
        }
        return m;
    }

    /** Strip scale (and any shear) from the rotation columns. */
    static Mat4 orthonormal(const Mat4& m) {
        Vec3 x(m[0]), y(m[1]);
        const float lx = glm::length(x);
        if (lx < 1e-8f) return glm::translate(Mat4(1.0f), Vec3(m[3]));
        x /= lx;
        y -= x * glm::dot(x, y);
        const float ly = glm::length(y);
        if (ly < 1e-8f) return glm::translate(Mat4(1.0f), Vec3(m[3]));
        y /= ly;
        // Always right-handed, so the result is a proper rotation that
        // quat_cast can read.
        const Vec3 z = glm::cross(x, y);
        Mat4 out(1.0f);
        out[0] = Vec4(x, 0.0f);
        out[1] = Vec4(y, 0.0f);
        out[2] = Vec4(z, 0.0f);
        out[3] = m[3];
        return out;
    }

    /** Intent (or raw clip name) -> index into ``CAnimator::clips``, or -1.
     *
     *  Tripo's retargeter names the clips, so a mechanic asking for "walk"
     *  has no way to know it is called "Walk" or "preset_walk_01" in this
     *  particular GLB -- the intent map from asset_animation.json is what
     *  closes that gap. Falls through to an exact-name match so
     *  hand-authored assets and direct clip names still work. */
    int clip_index(std::uint32_t entity, const char* clip_name) const {
        if (clip_name == nullptr) return -1;
        const World& w = app_->world_;
        if (!w.valid(entity) || !w.has<CAnimator>(entity)) return -1;

        std::string wanted = clip_name;
        if (w.has<CMesh>(entity)) {
            const auto& anims = w.get<CMesh>(entity).animations;
            const auto it = anims.find(wanted);
            if (it != anims.end()) wanted = it->second;
        }

        const auto& anim = w.get<CAnimator>(entity);
        for (size_t i = 0; i < anim.clips.size(); ++i) {
            if (anim.clips[i].name == wanted) return static_cast<int>(i);
        }
        return -1;
    }

    /** Shared guard: valid entity, real skeleton, in-range joint, and an
     *  animator to record the override flag on (created on demand so a purely
     *  procedural rig does not need one authored in the scene). */
    bool resolve(std::uint32_t entity, int joint, CSkeleton** out_sk,
                 CAnimator** out_anim) {
        if (!has_skeleton(entity)) return false;
        World& w = app_->world_;
        auto& sk = w.get<CSkeleton>(entity);
        const size_t n = sk.skeleton.joint_count();
        if (joint < 0 || static_cast<size_t>(joint) >= n) return false;
        if (sk.local_pose.size() != n) sk.local_pose = sk.skeleton.bind_pose;
        if (!w.has<CAnimator>(entity)) w.emplace_or_replace<CAnimator>(entity);
        auto& anim = w.get<CAnimator>(entity);
        if (anim.joint_overridden.size() != n) {
            anim.joint_overridden.resize(n, 0u);
        }
        *out_sk = &sk;
        *out_anim = &anim;
        return true;
    }

    /** Normalized joint name -> index for one skeleton, built once.
     *
     *  Keyed on the SkeletonData address because a rig's names never change
     *  after load, and the alternative -- normalizing 65 joint names on every
     *  slot lookup -- turns attach-time resolution into needless string
     *  churn. Cleared when a scene unloads via ``forget_name_caches``. */
    const std::unordered_map<std::string, int>& normalized_names_for(
        const gltf_mesh_load::SkeletonData& sk) const {
        const auto* key = &sk;
        const auto cached = name_cache_.find(key);
        if (cached != name_cache_.end()) return cached->second;

        std::unordered_map<std::string, int> built;
        built.reserve(sk.names.size());
        for (size_t i = 0; i < sk.names.size(); ++i) {
            std::string n = normalize_joint_name(sk.names[i]);
            if (n.empty()) continue;
            // First joint wins: rigs occasionally carry a duplicate name and
            // the earlier one is the one the skin actually references.
            built.emplace(std::move(n), static_cast<int>(i));
        }
        return name_cache_.emplace(key, std::move(built)).first->second;
    }

public:
    /** Drop cached name maps. Called on scene teardown so a reloaded scene
     *  cannot resolve slots against a freed skeleton's names. */
    void forget_name_caches() { name_cache_.clear(); }

private:
    Application* app_;
    mutable std::unordered_map<const gltf_mesh_load::SkeletonData*,
                               std::unordered_map<std::string, int>>
        name_cache_;
};

void Application::create_hand_entities() {
    const char* names[2] = {"hand_left", "hand_right"};
    hand_grip_to_entity_ = {Mat4(1.0f), Mat4(1.0f)};
    finger_targets_ = {entt::null, entt::null};

    // Full Player Avatar: the avatar's own hands ARE the player's hands.
    // Create invisible anchors only -- grab, sockets and haptics still need
    // an entity per hand -- so a second pair of hands can never appear on
    // top of the body's. builtin.avatar_body points the finger targets at
    // the avatar once it has found its finger bones.
    if (scene_.hands_source == "avatar") {
    for (int h = 0; h < 2; ++h) {
            const auto e = world_.create_entity(std::string("__") + names[h]);
            world_.registry().emplace<CTransform>(e, CTransform{});
            world_.registry().emplace<CTag>(
                e, CTag{{"player_hand", names[h], "player_system"}});
            hand_entities_[static_cast<size_t>(h)] = e;
        }
        PTV_INFO("Player hands come from the avatar; engine hands are anchors");
        return;
    }

    // Engine-owned assets, not project assets: built from
    // Default_Models/default_rigged_hand_model.glb by
    // engine/tools/prepare_default_hands.py. Absolute build path on desktop,
    // a bare APK-relative path on Quest -- same split as PTV_SHADER_DIR.
    const std::string base = std::string(PTV_ENGINE_ASSET_DIR);
    const char* files[2] = {"default_hand_left", "default_hand_right"};

    for (int h = 0; h < 2; ++h) {
        const std::string path = base + "/" + files[h] + ".glb";
        if (!asset_source().exists(path)) {
            PTV_WARN(
                "default hand mesh not found ({}); falling back to procedural "
                "block hands",
                path);
            return;
        }

        gltf_mesh_load::SkeletonData skel;
        // The source is authored in arbitrary units (a Sketchfab export at
        // 100x), so normalise it to a real hand's length.
        const float scale_m = std::clamp(scene_.hands_scale_m, 0.08f, 0.5f);
        const int slot = renderer_.load_glb_skinned(path, scale_m, skel);
        if (slot < 0 || skel.joint_count() == 0) {
            PTV_WARN("default hand mesh {} failed to load", path);
            return;
        }

        const auto e = world_.create_entity(std::string("__") + names[h]);
        world_.registry().emplace<CTransform>(e, CTransform{});

        CMesh mesh{};
        mesh.asset_id = files[h];
        mesh.glb_path = path;
        mesh.target_scale_meters = scale_m;
        mesh.skinned = true;
        world_.registry().emplace<CMesh>(e, std::move(mesh));

        CSkeleton cs{};
        cs.local_pose = skel.bind_pose;
        cs.skeleton = std::move(skel);
        gltf_mesh_load::build_joint_palette(cs.skeleton, cs.local_pose,
                                            cs.palette);
        world_.registry().emplace<CSkeleton>(e, std::move(cs));
        world_.registry().emplace<CAnimator>(e, CAnimator{});

        // Tagged so gameplay can recognise them, and deliberately NOT tagged
        // grabbable: a hand is not a prop.
        world_.registry().emplace<CTag>(
            e, CTag{{"player_hand", names[h]}});

        hand_entities_[static_cast<size_t>(h)] = e;

        // Measure the mesh's own palm frame and map it onto the OpenXR grip
        // frame, instead of assuming the asset was authored in grip space.
        if (skeleton_svc_) {
            const auto rig = hand_rig::analyze(
                *skeleton_svc_, static_cast<std::uint32_t>(e), h);
            bool plausible = rig.valid && glm::length(Vec3(rig.grip_to_entity[3])) < 0.3f;
            for (int c = 0; c < 4 && plausible; ++c) {
                for (int r = 0; r < 4; ++r) plausible = plausible && std::isfinite(rig.grip_to_entity[c][r]);
            }
            if (rig.valid && !plausible) {
                // A hand drawn metres from its controller is a hand the
                // player never sees; held on the grip it is at least there.
                PTV_WARN("default hand {}: grip alignment is implausible "
                         "(offset {:.2f} m); using the grip frame as-is", names[h],
                         glm::length(Vec3(rig.grip_to_entity[3])));
            }
            if (plausible) {
                hand_grip_to_entity_[static_cast<size_t>(h)] =
                    rig.grip_to_entity;
                // The wrist, seen from the grip frame: palm -> wrist is the
                // forearm's direction for a hand with no arm attached.
                hand_wrist_in_grip_[static_cast<size_t>(h)] =
                    Vec3((rig.grip_to_entity * rig.wrist_bind_model)[3]);
                finger_targets_[static_cast<size_t>(h)] = e;
            } else if (!rig.valid) {
                PTV_WARN("default hand {} has no recognisable fingers",
                         names[h]);
            }
        }

        // Drawn size, from the bind palette: a hand loaded at the wrong scale
        // is invisible (20 m: the player stands inside it), so it is dropped
        // for the procedural block hands, which are at least seen.
        {
            const auto& cs = world_.registry().get<CSkeleton>(e);
            Vec3 lo(1e9f), hi(-1e9f);
            bool finite = true;
            for (const auto& m : cs.palette) {
                const Vec3 t(m[3]);
                finite = finite && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z);
                lo = glm::min(lo, t);
                hi = glm::max(hi, t);
            }
            const Vec3 ext = hi - lo;
            const float span = std::max(ext.x, std::max(ext.y, ext.z));
            if (!finite || span > 0.6f || span < 0.02f) {
                PTV_WARN("default hand {} would be drawn {:.2f} m across (want ~0.2); "
                         "using block hands instead", names[h], span);
                world_.registry().destroy(e);
                hand_entities_[static_cast<size_t>(h)] = entt::null;
                finger_targets_[static_cast<size_t>(h)] = entt::null;
                continue;
            }
        }
    }

    if (hand_entities_[0] == entt::null || hand_entities_[1] == entt::null) return;
    PTV_INFO("Default rigged hands ready ({} joints each)",
             world_.registry().get<CSkeleton>(hand_entities_[0])
                 .skeleton.joint_count());
}

void Application::log_hands_telemetry(const bool tracked[2]) {
    // What the player can see of their hands, once a second: the Hands Only
    // hands once loaded at 20 m long and drew nothing visible, with no line
    // in the log to say so.
    const char* side[2] = {"L", "R"};
    std::string line;
    for (int h = 0; h < 2; ++h) {
        const auto e = hand_entities_[static_cast<size_t>(h)];
        const auto id = static_cast<std::uint32_t>(e);
        const bool has = e != entt::null && world_.valid(id);
        const bool mesh = has && world_.registry().all_of<CMesh>(e) &&
                          !world_.registry().get<CMesh>(e).glb_path.empty();
        bool finite = true;
        float size_m = 0.0f;
        if (mesh && world_.registry().all_of<CSkeleton>(e)) {
            const auto& cs = world_.registry().get<CSkeleton>(e);
            // Drawn extent: the spread of the joint palette's translations
            // (a 0.2 m hand spans ~0.15 m of joints).
            Vec3 lo(1e9f), hi(-1e9f);
            for (const auto& m : cs.palette) {
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) finite = finite && std::isfinite(m[c][r]);
                }
                const Vec3 t(m[3]);
                lo = glm::min(lo, t);
                hi = glm::max(hi, t);
            }
            if (!cs.palette.empty()) {
                const Vec3 ext = hi - lo;
                size_m = std::max(ext.x, std::max(ext.y, ext.z));
            }
        }
        const Vec3 pos = has && world_.registry().all_of<CTransform>(e)
                             ? world_.registry().get<CTransform>(e).t.position
                             : Vec3(0.0f);
        line += fmt::format(
            "{}{}: tracked={} mesh={} drawn={} finite={} joint_span_m={:.3f} head_m={:.2f}",
            h == 0 ? "" : " | ", side[h], tracked[h] ? 1 : 0, mesh ? 1 : 0,
            has && renderer_.drew_entity(id) ? 1 : 0, finite ? 1 : 0, size_m,
            glm::length(pos - last_head_world_pos_));
    }
    PTV_INFO("hands: source={} {}", scene_.hands_source, line);
}

float Application::player_body_radius() const {
    return scene_.player_body_radius_m > 1e-3f ? scene_.player_body_radius_m
                                               : kPlayerBodyRadius;
}

void Application::create_player_body() {
    if (player_entity_ != entt::null) return;

    player_capsule_height_ = std::clamp(scene_.player_height_meters, 0.6f,
                                        2.6f);

    // The scene's own player rig, when it has one, IS the player. A second
    // engine-made "__player" next to it meant two entities tagged "player":
    // the rig stood still at its authored spot while the real body moved, so
    // generated code that looked the player up by name chased the wrong one,
    // and health bound to the rig never saw the damage enemies dealt.
    entt::entity e = find_adoptable_player_rig(world_);
    const bool adopted = e != entt::null;
    if (adopted) {
        // Authored colliders on the rig are replaced by the player capsule.
        physics_.remove_body(static_cast<std::uint32_t>(e));
        world_.registry().remove<CPhysics>(e);
        auto& tags = world_.registry().get<CTag>(e).tags;
        if (std::find(tags.begin(), tags.end(), "player") == tags.end()) {
            tags.push_back("player");
        }
    } else {
        e = world_.create_entity("__player");
    }
    CTransform ct{};
    // Until the first HMD pose arrives the capsule stands on the spawn point.
    // World space, via rig_world_position: the rig's own coordinates are
    // frame-local, and a body placed with those while the player is riding
    // something would collide against the wrong part of the world.
    {
        const Vec3 base = rig_world_position();
        ct.t.position =
            Vec3(base.x, base.y + 0.5f * player_capsule_height_, base.z);
    }
    // No CMesh on this entity, so the capsule builder reads its height from
    // the transform scale.
    ct.t.scale = player_capsule_height_;
    world_.registry().emplace_or_replace<CTransform>(e, ct);

    CPhysics cp{};
    // Kinematic, not dynamic: the user's real movement drives this body, and
    // gravity or an impulse must never shove the headset somewhere the user
    // did not go. Kinematic still collides with everything else, which is the
    // whole point -- an enemy can now run into the player.
    cp.kind = "kinematic";
    cp.mass_kg = 70.0f;
    cp.collision_shape = "capsule";
    cp.friction = 0.0f;
    cp.restitution = 0.0f;
    // The same radius as the walking body: the body the player sees.
    cp.capsule_radius_m = player_body_radius();
    world_.registry().emplace<CPhysics>(e, cp);

    if (!adopted) world_.registry().emplace<CTag>(e, CTag{{"player"}});

    // The player is an ordinary combatant. Without health nothing can hurt
    // them, and without a faction an enemy cannot tell them apart from a
    // crate -- both of which the "ducks damage the player" case needs. An
    // adopted rig keeps whatever health and faction the scene authored.
    if (!world_.registry().all_of<CHealth>(e)) {
        CHealth hp{};
        hp.current = hp.max = 100.0f;
        world_.registry().emplace<CHealth>(e, hp);
    }
    if (!world_.registry().all_of<CFaction>(e)) {
        CFaction faction{};
        faction.id = "player";
        faction.hostile_to = {"enemy"};
        world_.registry().emplace<CFaction>(e, std::move(faction));
    }

    player_entity_ = e;
    PTV_INFO("Player entity: {} ({})", static_cast<std::uint32_t>(e),
             adopted ? "the scene's player_rig" : "engine-made __player");
    player_capsule_center_ = ct.t.position;
    physics_.add_body_for(world_, static_cast<std::uint32_t>(e));
    // The player's body touches the world and NPCs, never a prop.
    physics_.set_body_class(static_cast<std::uint32_t>(e),
                            PhysicsWorld::BodyClass::PlayerBody);

    // The body that actually walks. Feet on the spawn point until the first
    // headset pose moves it under the player's head.
    physics_.create_player_character(rig_world_position(),
                                     player_capsule_height_,
                                     player_body_radius());
    PTV_INFO("Player body radius {:.3f} m ({})", player_body_radius(),
             scene_.player_body_radius_m > 0.0f ? "from the scene" : "default");
    // Fresh per scene: the previous scene's part entities went with its world.
    player_parts_ = std::make_unique<PlayerParts>();
    player_parts_->configure(scene_.hands_source == "avatar");
    motor_needs_sync_ = true;
    motor_has_grounded_ = false;
    motor_virtual_floor_ = false;
    {
        // A world with nothing to stand on under the spawn point would drop
        // the player forever. Fall back to the flat floor plane there, which
        // is what every scene assumed before the player had gravity.
        const Vec3 base = rig_world_position();
        Vec3 hit(0.0f), n(0.0f, 1.0f, 0.0f);
        motor_virtual_floor_ = !physics_.raycast(
            base + Vec3(0.0f, 1.5f, 0.0f), Vec3(0.0f, -1.0f, 0.0f), 50.0f, hit,
            n);
        if (motor_virtual_floor_) {
            PTV_WARN(
                "Nothing collidable under the spawn point; the player will "
                "stand on the flat floor plane y={:.2f} instead of falling",
                teleport_floor_y_);
        }
    }
    PTV_INFO("Player collision capsule ready ({:.2f} m)",
             player_capsule_height_);
}

bool Application::motor_active() const {
    // The motor walks a human-sized body in world space. A player riding a
    // moving frame, or shrunk to fit inside something, keeps the older
    // frame-relative movement: sweeping a full-size capsule there would
    // collide with the very vehicle they are standing in.
    return physics_.has_player_character() && player_entity_ != entt::null &&
           !has_reference_frame() && std::abs(rig_.scale - 1.0f) < 1.0e-3f;
}

Vec3 Application::current_head_world() const {
    const Mat4 rig = rig_world_matrix();
    if (head_local_valid_) return mat_translation(rig * head_local_);
    // No pose yet: a standing player's eyes above the stage origin.
    const Vec3 base = rig_world_position();
    return base + Vec3(0.0f, 0.93f * player_capsule_height_, 0.0f);
}

void Application::apply_pending_move_legacy() {
    Vec3 d = pending_move_;
    pending_move_ = Vec3(0.0f);
    pending_jump_mps_ = 0.0f;
    if (glm::length(d) < 1.0e-7f) return;
    if (player_entity_ != entt::null) {
        collect_player_body_family(motor_ignored_);
        d = physics_.slide_character(player_capsule_center_,
                                     player_capsule_height_, d,
                                     motor_ignored_);
    }
    const Vec3 local = world_vector_to_frame(d);
    rig_.pos_x += local.x;
    rig_.pos_y += local.y;
    rig_.pos_z += local.z;
}

void Application::collect_player_body_family(
    std::vector<std::uint32_t>& out) const {
    out.clear();
    auto& reg = world_.registry();
    auto add = [&](entt::entity e) {
        if (e == entt::null || !reg.valid(e)) return;
        const auto id = static_cast<std::uint32_t>(e);
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    };
    add(player_entity_);
    for (const auto e : hand_entities_) add(e);
    for (const auto& hand : hands_) add(hand.held);

    // The player's own body entities, by tag: tagged once at scene load, so
    // checking the tag on each attachment's chain is cheaper than a scan.
    auto is_body_root = [&](entt::entity e) {
        if (e == player_entity_) return true;
        if (e == hand_entities_[0] || e == hand_entities_[1]) return true;
        if (e == hands_[0].held || e == hands_[1].held) return true;
        const auto* tag = reg.try_get<CTag>(e);
        if (tag == nullptr) return false;
        for (const auto& t : tag->tags) {
            if (t == "player_avatar" || t == "player_rig") return true;
        }
        return false;
    };

    {
        auto view = reg.view<const CAttachment>();
        for (const auto child : view) {
            entt::entity p = static_cast<entt::entity>(
                view.get<const CAttachment>(child).parent);
            for (int depth = 0; depth < 8 && p != entt::null && reg.valid(p);
                 ++depth) {
                if (is_body_root(p)) {
                    add(child);
                    break;
                }
                const auto* up = reg.try_get<CAttachment>(p);
                if (up == nullptr) break;
                p = static_cast<entt::entity>(up->parent);
            }
        }
    }
    {
        auto view = reg.view<const CEquipped>();
        for (const auto item : view) {
            const auto wearer = static_cast<entt::entity>(
                view.get<const CEquipped>(item).wearer);
            if (wearer != entt::null && reg.valid(wearer) &&
                is_body_root(wearer)) {
                add(item);
            }
        }
    }
}

void Application::step_player_motor(float dt) {
    const bool grounded = vertical_mode_ == ::ptv::VerticalMode::Grounded;
    Vec3 request = pending_move_;
    const float jump = pending_jump_mps_;
    pending_move_ = Vec3(0.0f);
    pending_jump_mps_ = 0.0f;
    if (grounded) request.y = 0.0f;

    // Everything that moves with the player would otherwise block or shove
    // the body: picking up a rifle must not push the player backwards, and a
    // worn holster overlapping the capsule must not push them forwards at
    // their own speed every frame (which compounds into a runaway).
    collect_player_body_family(motor_ignored_);
    physics_.set_player_character_ignored(motor_ignored_);

    Vec3 head = current_head_world();
    if (motor_needs_sync_ && head_local_valid_) {
        // First real pose (or a spawn): stand the body under the head.
        const Vec3 rig = rig_world_position();
        physics_.teleport_player_character(Vec3(head.x, rig.y, head.z));
        motor_head_resolved_ = head;
        motor_needs_sync_ = false;
    }

    // --- (a) Stick movement, swept through the body --------------------
    // Gravity and ground contact are integrated here, once per frame.
    const Vec3 feet_before = physics_.player_character_state().feet;
    Vec3 moved = physics_.move_player_character(request, grounded, jump, dt);
    {
        // Governor: the body may cover what was asked for, plus what a
        // moving platform carried it, plus a little penetration recovery.
        // Anything more is something pushing the player that should not be.
        const auto st0 = physics_.player_character_state();
        const float asked = glm::length(Vec3(request.x, 0.0f, request.z));
        const float carried =
            glm::length(Vec3(st0.ground_velocity.x, 0.0f,
                             st0.ground_velocity.z)) * dt;
        const float allowed = asked + carried + std::max(0.01f, 1.0f * dt);
        const Vec3 flat(moved.x, 0.0f, moved.z);
        const float got = glm::length(flat);
        if (got > allowed && got > 1.0e-5f) {
            const Vec3 kept = flat * (allowed / got);
            if (!motor_push_warned_ && (got - allowed) > 0.2f * got) {
                motor_push_warned_ = true;
                physics_.player_character_contacts(motor_contacts_);
                std::string names;
                for (const std::uint32_t c : motor_contacts_) {
                    const auto ce = static_cast<entt::entity>(c);
                    if (!world_.registry().valid(ce)) continue;
                    const std::string* id = world_.id_of(ce);
                    if (!names.empty()) names += ", ";
                    names += (id != nullptr && !id->empty())
                                 ? *id
                                 : std::to_string(c);
                }
                PTV_WARN(
                    "player motor: body was pushed {:.2f} m this frame but "
                    "only {:.2f} m was asked for; clamped. Touching: [{}]. "
                    "Something that follows the player has a collider; attach "
                    "it to the player/avatar so the body ignores it.",
                    got, allowed, names.empty() ? "nothing" : names);
            }
            moved = Vec3(kept.x, moved.y, kept.z);
            physics_.set_player_character_position(feet_before + moved);
        }
    }
    {
        const Vec3 horizontal(moved.x, grounded ? 0.0f : moved.y, moved.z);
        const Vec3 local = world_vector_to_frame(horizontal);
        rig_.pos_x += local.x;
        rig_.pos_y += local.y;
        rig_.pos_z += local.z;
    }

    // --- (b) The body follows the head's physical walking --------------
    head = current_head_world();
    auto st = physics_.player_character_state();
    const Vec3 follow(head.x - st.feet.x, 0.0f, head.z - st.feet.z);
    const float follow_len = glm::length(follow);
    if (follow_len > 1.0e-4f) {
        if (follow_len > 1.0f) {
            // The player walked around an obstacle the body could not pass
            // (a table they leaned across, say) and is now well clear of it.
            // Stand the body back under them rather than dragging it along.
            physics_.teleport_player_character(
                Vec3(head.x, st.feet.y, head.z));
        } else {
            physics_.move_player_character(follow, true, 0.0f, dt,
                                           /*apply_gravity=*/false);
        }
        st = physics_.player_character_state();
    }

    // --- (c) Keep the head out of walls ---------------------------------
    // Static geometry only, so leaning over a crate or a table is still
    // possible. The head sweeps from where it was last allowed to be, so
    // even a fast lunge cannot tunnel through a thin wall.
    if (head_local_valid_) {
        const Vec3 allowed =
            physics_.resolve_head(motor_head_resolved_, head, kHeadRadius);
        const Vec3 push(allowed.x - head.x, 0.0f, allowed.z - head.z);
        if (glm::length(push) > 1.0e-5f) {
            const Vec3 local = world_vector_to_frame(push);
            rig_.pos_x += local.x;
            rig_.pos_z += local.z;
            head += push;
        }
        motor_head_resolved_ = Vec3(head.x, allowed.y, head.z);
    }

    // --- (d) The floor follows the ground under the body ----------------
    if (grounded) {
        Vec3 feet = st.feet;
        if (motor_virtual_floor_ && feet.y < teleport_floor_y_) {
            feet.y = teleport_floor_y_;
            physics_.teleport_player_character(feet);
        }
        if (st.grounded) {
            motor_last_grounded_ = feet;
            motor_has_grounded_ = true;
        } else if (motor_has_grounded_ &&
                   feet.y < motor_last_grounded_.y - 30.0f) {
            // Fell out of the world through a gap in the level. Put them
            // back where they last stood instead of falling forever.
            PTV_WARN("Player fell out of the world; returning them to "
                     "their last footing");
            feet = motor_last_grounded_;
            physics_.teleport_player_character(feet);
        }
        const Vec3 rig = rig_world_position();
        set_rig_world_position(Vec3(rig.x, feet.y, rig.z));
    }

    // --- (e) Crouching resizes the body ---------------------------------
    motor_resize_timer_s_ -= dt;
    if (head_local_valid_ && motor_resize_timer_s_ <= 0.0f) {
        motor_resize_timer_s_ = 0.25f;
        const float want = std::clamp(head.y - st.feet.y + 0.10f, 0.8f,
                                      player_capsule_height_);
        if (std::abs(want - st.height) > 0.08f) {
            physics_.resize_player_character(want);
        }
    }

    // --- (f) The contact capsule rides with the body --------------------
    st = physics_.player_character_state();
    const Vec3 center = st.feet + Vec3(0.0f, 0.5f * player_capsule_height_, 0.0f);
    player_capsule_center_ = center;
    if (world_.registry().valid(player_entity_) &&
        world_.registry().all_of<CTransform>(player_entity_)) {
        world_.registry().get<CTransform>(player_entity_).t.position = center;
    }
    physics_.move_kinematic(static_cast<std::uint32_t>(player_entity_), center,
                            Quat(1.0f, 0.0f, 0.0f, 0.0f), dt);
}

Quat Application::grip_trim_quat() const {
    const Vec3 r = glm::radians(scene_.hands_grip_trim_deg);
    return glm::normalize(glm::quat(r));
}

void Application::draw_hand_debug_axes(const Vec3 grip_pos[2],
                                       const Quat grip_rot[2],
                                       const bool valid[2]) {
    if (!scene_.hands_debug_axes || !draw_svc_) return;
    for (int h = 0; h < 2; ++h) {
        if (!valid[h]) continue;
        const Quat g = grip_rot[h] * grip_trim_quat();
        constexpr float kLen = 0.08f;
        // Grip frame: X red, Y green, Z blue.
        draw_svc_->line(grip_pos[h], grip_pos[h] + g * Vec3(kLen, 0, 0),
                        Vec4(1, 0.2f, 0.2f, 1), 0.004f);
        draw_svc_->line(grip_pos[h], grip_pos[h] + g * Vec3(0, kLen, 0),
                        Vec4(0.2f, 1, 0.2f, 1), 0.004f);
        draw_svc_->line(grip_pos[h], grip_pos[h] + g * Vec3(0, 0, kLen),
                        Vec4(0.2f, 0.4f, 1, 1), 0.004f);
        // The aim direction the hand should agree with: -Z, longer, white.
        draw_svc_->line(grip_pos[h], grip_pos[h] + g * Vec3(0, 0, -0.2f),
                        Vec4(1, 1, 1, 1), 0.002f);
    }
}

void Application::configure_weapon_systems() {
    if (!effects_ || !projectiles_) return;
    const ScenePerformance& p = scene_.performance;
    EffectsBudget b{};
    b.max_quads = static_cast<std::size_t>(std::max(64, p.max_fx_quads));
    b.max_decals = static_cast<std::size_t>(std::max(0, p.max_decals));
    b.max_lights = static_cast<std::size_t>(std::max(0, p.max_flash_lights));
    effects_->configure(b);
    renderer_.set_fx_budget(b.max_quads);
    projectiles_->configure(p.max_live_projectiles, p.max_projectile_meshes);
    projectiles_->bind(ProjectileSystem::Services{&world_, physics_svc_.get(),
                                                  combat_svc_.get(), effects_.get(),
                                                  hands_svc_.get()});
    std::vector<ProjectileSystem::Preset> presets;
    for (const auto& e : scene_.projectiles) {
        ProjectileSystem::Preset pr{};
        pr.id = e.id;
        pr.template_name = e.template_entity;
        ProjectileDesc& d = pr.desc;
        d.velocity = Vec3(0.0f, 0.0f, -std::max(1.0f, e.speed_mps));
        d.gravity_scale = e.gravity_scale;
        d.drag = e.drag;
        d.radius_m = e.radius_m;
        d.lifetime_s = e.lifetime_s;
        d.splash_radius_m = e.splash_radius_m;
        d.splash_damage = e.splash_damage;
        d.on_hit = e.on_hit == "stick"     ? ProjectileOnHit::Stick
                   : e.on_hit == "bounce"  ? ProjectileOnHit::Bounce
                   : e.on_hit == "explode" ? ProjectileOnHit::Explode
                                           : ProjectileOnHit::Despawn;
        d.visual = e.kind == "energy_bolt" ? ProjectileVisual::Bolt : ProjectileVisual::Tracer;
        d.visual_width_m = std::max(0.004f, e.diameter_m * 1.2f);
        d.visual_length_m = std::max(0.05f, e.length_m);
        d.impact = e.on_hit == "explode" ? ImpactStyle::Explosion
                   : e.kind == "energy_bolt" ? ImpactStyle::Energy
                   : e.kind == "arrow"       ? ImpactStyle::Dust
                                             : ImpactStyle::Sparks;
        presets.push_back(std::move(pr));
    }
    projectiles_->set_presets(std::move(presets));
}

Vec3 Application::hand_forearm_direction(int hand) const {
    const auto h = static_cast<size_t>(hand);
    const Vec3 wrist = hand_wrist_in_grip_[h];
    // OpenXR grip +Y points from the palm back toward the wrist for a hand
    // closed on a controller; the rig's measured wrist refines it.
    const Vec3 local = glm::length(wrist) > 0.02f ? glm::normalize(wrist)
                                                  : Vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(last_grip_rot_[h] * local);
}

void Application::place_hand_entity(int hand, const Vec3& grip_pos,
                                    const Quat& grip_rot) {
    const auto he = hand_entities_[static_cast<size_t>(hand)];
    if (he == entt::null || !world_.registry().all_of<CTransform>(he)) return;
    // A non-finite finger pose draws the whole hand as nothing; put it back
    // to rest rather than lose the hand.
    if (auto* cs = world_.registry().try_get<CSkeleton>(he)) {
        bool finite = true;
        for (const auto& p : cs->local_pose) {
            finite = finite && std::isfinite(p.rotation.w) && std::isfinite(p.rotation.x) &&
                     std::isfinite(p.rotation.y) && std::isfinite(p.rotation.z) &&
                     std::isfinite(p.translation.x) && std::isfinite(p.translation.y) &&
                     std::isfinite(p.translation.z);
        }
        if (!finite) {
            PTV_WARN("hand {}: non-finite finger pose; reset to rest", hand == 0 ? "L" : "R");
            cs->local_pose = cs->skeleton.bind_pose;
        }
    }
    // grip_to_entity maps the OpenXR grip frame onto the hand mesh's own
    // frame, measured from the rig (HandRig) when the hands were loaded.
    // Identity for a mesh already authored in the grip frame.
    const Mat4 grip = glm::translate(Mat4(1.0f), grip_pos) *
                      glm::mat4_cast(grip_rot * grip_trim_quat());
    const Mat4 m = grip * hand_grip_to_entity_[static_cast<size_t>(hand)];
    auto& t = world_.registry().get<CTransform>(he).t;
    t.position = mat_translation(m);
    t.rotation = glm::normalize(glm::quat_cast(Mat3(m)));
}

void Application::update_player_body(const Vec3& head_world_position,
                                     float dt) {
    if (player_entity_ == entt::null) return;

    // Follow the headset horizontally so physically walking around the
    // playspace moves the collider too, but take the vertical extent from the
    // rig: the capsule represents a standing body on the floor, and ducking
    // should not shrink the player out of the world.
    // The floor level comes from the rig, in WORLD space -- the rig's own
    // coordinates are relative to whatever the player is riding.
    const Vec3 rig_world = rig_world_position();
    Vec3 center(head_world_position.x,
                rig_world.y + 0.5f * player_capsule_height_,
                head_world_position.z);
    if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
        !std::isfinite(center.z)) {
        center = Vec3(rig_world.x, rig_world.y + 0.5f * player_capsule_height_,
                      rig_world.z);
    }
    player_capsule_center_ = center;

    auto& t = world_.registry().get<CTransform>(player_entity_).t;
    t.position = center;
    physics_.move_kinematic(static_cast<std::uint32_t>(player_entity_), center,
                            Quat(1.0f, 0.0f, 0.0f, 0.0f), dt);
}

namespace {

/** Attach each named socket to the joint whose bind-pose position is closest.
 *
 *  ``asset_contract`` derives sockets from geometry alone -- it has no idea
 *  the mesh is rigged -- so they arrive as fixed offsets in the mesh's bind
 *  pose. For a static prop that is the whole truth. For a character it is
 *  only true on frame zero: an animated arm carries the muzzle with it, and a
 *  socket that ignores the skeleton stays behind in mid-air.
 *
 *  Nearest-joint is the right heuristic because these sockets sit ON the
 *  geometry they belong to -- a muzzle is at the end of a barrel that a hand
 *  is holding, so the closest joint IS the hand. Ambiguity only arises for a
 *  socket floating between two limbs, which the socket derivation does not
 *  produce.
 */
std::unordered_map<std::string, int> bind_sockets_to_joints(
    const std::unordered_map<std::string, Transform>& sockets,
    const gltf_mesh_load::SkeletonData& skel) {
    std::unordered_map<std::string, int> out;
    const size_t joints = skel.joint_count();
    if (sockets.empty() || joints == 0) return out;

    // Bind-pose world position of every joint, in the same metres the socket
    // transforms use. inverse_bind maps mesh space -> joint space, so its
    // inverse puts the joint back in mesh space; unit_scale then matches the
    // loader's rescale of the vertices.
    std::vector<Vec3> joint_pos(joints, Vec3(0.0f));
    for (size_t j = 0; j < joints; ++j) {
        const Mat4 bind = glm::inverse(skel.inverse_bind[j]);
        joint_pos[j] = Vec3(bind[3]) * skel.unit_scale;
    }

    for (const auto& [name, xform] : sockets) {
        const Vec3 p = xform.position;
        int best = -1;
        float best_d2 = 0.0f;
        for (size_t j = 0; j < joints; ++j) {
            const Vec3 d = joint_pos[j] - p;
            const float d2 = glm::dot(d, d);
            if (best < 0 || d2 < best_d2) {
                best = static_cast<int>(j);
                best_d2 = d2;
            }
        }
        if (best >= 0) out.emplace(name, best);
    }
    return out;
}

}  // namespace

namespace {

/** Loose joint-name matching for the handful of anchors generated code can
 *  reasonably ask for without ever seeing the asset.
 *
 *  Tripo names the bones, and the names differ between rig models, so an
 *  exact match on "hand_r" would fail on a rig that calls it "RightHand" or
 *  "mixamorig:RightHand". Each alias is a set of substrings that must ALL
 *  appear (case-insensitively) in the joint name, which is specific enough
 *  that "hand" + "r" does not match "Forearm_L". */
struct JointAlias {
    const char* anchor;
    const char* must_contain_a;
    const char* must_contain_b;  // nullptr when only one term is needed
};

constexpr JointAlias kJointAliases[] = {
    {"hand_r", "hand", "r"},   {"hand_l", "hand", "l"},
    {"right_hand", "hand", "r"}, {"left_hand", "hand", "l"},
    {"head", "head", nullptr}, {"spine", "spine", nullptr},
    {"chest", "chest", nullptr}, {"hips", "hip", nullptr},
    {"root", "root", nullptr},
};

std::string lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/** Does ``joint`` look like the side-specific bone ``alias`` is after?
 *
 *  The side term is matched as a standalone token ("_r", ".r", "right") so a
 *  left-hand bone never satisfies a right-hand request. */
bool joint_matches_side(const std::string& joint_lower, const char* side) {
    const std::string s(side);
    if (s == "r") {
        return joint_lower.find("right") != std::string::npos ||
               joint_lower.find("_r") != std::string::npos ||
               joint_lower.find(".r") != std::string::npos ||
               (joint_lower.size() > 1 &&
                joint_lower.compare(joint_lower.size() - 1, 1, "r") == 0);
    }
    if (s == "l") {
        return joint_lower.find("left") != std::string::npos ||
               joint_lower.find("_l") != std::string::npos ||
               joint_lower.find(".l") != std::string::npos ||
               (joint_lower.size() > 1 &&
                joint_lower.compare(joint_lower.size() - 1, 1, "l") == 0);
    }
    return joint_lower.find(s) != std::string::npos;
}

}  // namespace

Application::AttachAnchor Application::resolve_attach_anchor(
    std::uint32_t parent, const std::string& name) const {
    AttachAnchor out{};
    if (name.empty() || !world_.valid(parent)) return out;

    // 1) A named socket. On a rigged parent these already ride the animated
    //    skeleton, so this is the best answer when it exists.
    if (world_.has<CMesh>(parent)) {
        const auto& sockets = world_.get<CMesh>(parent).sockets;
        if (sockets.find(name) != sockets.end()) {
            out.valid = true;
            out.is_socket = true;
            return out;
        }
    }

    if (!world_.has<CSkeleton>(parent)) return out;
    const auto& skel = world_.get<CSkeleton>(parent).skeleton;

    // 2) An exact joint name.
    if (const auto it = skel.name_to_joint.find(name);
        it != skel.name_to_joint.end()) {
        out.valid = true;
        out.joint = it->second;
        return out;
    }

    // 3) A canonical body slot. Added so "forearm_r" or "index_1_l" works as
    //    an attach anchor directly, which is what lets equip name a place on
    //    a body without first translating it into whatever the rig happened
    //    to call that bone. Covers Mixamo, Tripo, Unity and Unreal naming in
    //    one lookup; see ptv/api/BodySlots.h.
    if (const auto& candidates = body_slot_candidates(name);
        !candidates.empty()) {
        for (const auto& candidate : candidates) {
            for (size_t j = 0; j < skel.names.size(); ++j) {
                if (normalize_joint_name(skel.names[j]) != candidate) continue;
                out.valid = true;
                out.joint = static_cast<int>(j);
                return out;
            }
        }
    }

    // 4) A well-known alias, matched loosely against the rig's own names.
    const std::string wanted = lower_ascii(name);
    for (const auto& alias : kJointAliases) {
        if (wanted != alias.anchor) continue;
        for (size_t j = 0; j < skel.names.size(); ++j) {
            const std::string joint = lower_ascii(skel.names[j]);
            if (joint.find(alias.must_contain_a) == std::string::npos) {
                continue;
            }
            if (alias.must_contain_b != nullptr &&
                !joint_matches_side(joint, alias.must_contain_b)) {
                continue;
            }
            out.valid = true;
            out.joint = static_cast<int>(j);
            return out;
        }
    }
    return out;
}

void Application::tick_attachments(float dt) {
    auto view = world_.registry().view<CAttachment, CTransform>();
    for (auto child : view) {
        const auto& att = view.get<CAttachment>(child);
        const auto parent = att.parent;
        if (!world_.valid(parent) || !world_.has<CTransform>(parent)) {
            // The parent died out from under it. Leave the child where it is
            // rather than teleporting it to the origin.
            continue;
        }

        Mat4 anchor_world(1.0f);
        const AttachAnchor anchor = resolve_attach_anchor(parent, att.anchor);
        if (anchor.is_socket && sockets_svc_) {
            anchor_world = sockets_svc_->socket_world(parent,
                                                      att.anchor.c_str());
        } else if (anchor.valid && anchor.joint >= 0 &&
                   world_.has<CSkeleton>(parent)) {
            const auto& sk = world_.get<CSkeleton>(parent);
            const auto j = static_cast<size_t>(anchor.joint);
            // palette[j] moves bind-pose mesh space to the posed position, so
            // composing it with the joint's bind position lands on the joint
            // wherever the animation has taken it.
            Mat4 bind(1.0f);
            if (j < sk.skeleton.inverse_bind.size()) {
                bind = glm::inverse(sk.skeleton.inverse_bind[j]);
                bind[3] = glm::vec4(Vec3(bind[3]) * sk.skeleton.unit_scale,
                                    1.0f);
            }
            Mat4 skin(1.0f);
            if (j < sk.palette.size()) skin = sk.palette[j];
            // The joint contributes where it is and which way it faces,
            // never its scale: offsets are metres on every rig.
            anchor_world = world_.get<CTransform>(parent).t.matrix() *
                           rigid_part(skin * bind);
        } else {
            anchor_world = world_.get<CTransform>(parent).t.matrix();
        }

        const Mat4 offset =
            glm::translate(Mat4(1.0f), att.local_position) *
            glm::mat4_cast(att.local_rotation);
        const Mat4 world = anchor_world * offset;

        auto& t = view.get<CTransform>(child).t;
        t.position = Vec3(world[3]);
        t.rotation = glm::normalize(glm::quat_cast(Mat4(glm::mat3(
            glm::normalize(Vec3(world[0])), glm::normalize(Vec3(world[1])),
            glm::normalize(Vec3(world[2]))))));

        const auto child_id = static_cast<std::uint32_t>(child);
        if (physics_.has_body(child_id)) {
            physics_.move_kinematic(child_id, t.position, t.rotation, dt);
        }
    }
}

// ---------------------------------------------------------------------------
// Player reference frame and scale
// ---------------------------------------------------------------------------

namespace {

/** Scale bounds.
 *
 *  The lower bound is not arbitrary: below roughly a centimetre the player's
 *  interpupillary distance shrinks past what the stereo pair can resolve
 *  meaningfully, and the near plane (which scales with them) collapses toward
 *  zero and wrecks depth precision. The upper bound keeps a giant's near plane
 *  from clipping through everything within arm's reach. */
constexpr float kMinPlayerScale = 0.01f;
constexpr float kMaxPlayerScale = 100.0f;

/** Yaw of a world matrix, discarding pitch and roll.
 *
 *  Must be the exact inverse of ``glm::rotate(m, yaw, +Y)``, because the rig
 *  composes with that and this decomposes it: any mismatch makes boarding a
 *  moving frame spin the player. With forward at -Z, ``R_y(t)*(0,0,-1)`` is
 *  ``(-sin t, 0, -cos t)``, so recovering ``t`` needs BOTH components
 *  negated -- ``atan2(-fwd.x, -fwd.z)``, matching the derivation documented
 *  on the locomotion menu's anchor. */
float matrix_yaw(const Mat4& m) {
    const Vec3 fwd = Vec3(m * Vec4(0.0f, 0.0f, -1.0f, 0.0f));
    const Vec3 flat(fwd.x, 0.0f, fwd.z);
    if (glm::length(flat) < 1e-5f) return 0.0f;
    return yaw_from_forward(flat);
}

/** Rig yaw that makes the player face ``facing``.
 *
 *  Both spawn paths used to write ``atan2(f.x, -f.z)``, which is the heading
 *  NEGATED: it agrees with the correct value only when ``f.x`` is zero, so a
 *  spawn facing straight down -Z looked right and every other facing put the
 *  player in mirrored orientation. That went unnoticed while the only spawn
 *  was a scene's default; named portal spawn points, which routinely face
 *  sideways, make it the common case. */
float yaw_for_facing(const Vec3& facing) {
    return yaw_from_forward(facing);
}

}  // namespace

Mat4 Application::rig_parent_matrix() const {
    if (rig_.parent == entt::null ||
        !world_.registry().valid(rig_.parent)) {
        return Mat4(1.0f);
    }

    Mat4 anchor(1.0f);
    bool have_anchor = false;

    // An anchor names a joint or socket, so a passenger inside a head rides
    // the ANIMATED skull rather than the entity's root transform -- which for
    // a walking character is down at its feet and does not turn with the neck.
    if (!rig_.parent_anchor.empty()) {
        const auto self = static_cast<std::uint32_t>(rig_.parent);
        if (skeleton_svc_ != nullptr) {
            const int joint = skeleton_svc_->joint_for_slot(
                self, rig_.parent_anchor.c_str());
            if (joint >= 0) {
                anchor = skeleton_svc_->joint_world(self, joint);
                have_anchor = true;
            }
        }
        // Sockets are the fallback, so an anchor can also be a derived
        // attachment point ("cockpit", "grip") on an unrigged vehicle.
        if (!have_anchor && sockets_svc_ != nullptr &&
            sockets_svc_->has_socket(self, rig_.parent_anchor.c_str())) {
            anchor =
                sockets_svc_->socket_world(self, rig_.parent_anchor.c_str());
            have_anchor = true;
        }
    }

    if (!have_anchor) {
        if (!world_.registry().all_of<CTransform>(rig_.parent)) {
            return Mat4(1.0f);
        }
        const auto& t = world_.registry().get<CTransform>(rig_.parent).t;
        anchor = glm::translate(Mat4(1.0f), t.position) *
                 glm::mat4_cast(t.rotation);
    }

    // Position plus heading only. See the header: inheriting a parent's pitch
    // or roll tilts the player's horizon, which is the most reliable way to
    // make someone sick in VR.
    return glm::rotate(glm::translate(Mat4(1.0f), mat_translation(anchor)),
                       matrix_yaw(anchor), Vec3(0.0f, 1.0f, 0.0f));
}

Mat4 Application::rig_world_matrix() const {
    // parent -> local offset -> scale. Scale is innermost so it shrinks the
    // player's own body and their tracked motion, NOT their offset within the
    // frame: a passenger two metres along a deck is two metres along it
    // whatever size they are.
    const Mat4 local = rig_world_from_stage(rig_.pos_x, rig_.pos_y, rig_.pos_z,
                                            rig_.yaw_rad);
    Mat4 m = rig_parent_matrix() * local;
    if (rig_.scale != 1.0f) {
        m = glm::scale(m, Vec3(rig_.scale));
    }
    return m;
}

Vec3 Application::rig_world_position() const {
    const Vec3 local(rig_.pos_x, rig_.pos_y, rig_.pos_z);
    if (!has_reference_frame()) return local;
    return Vec3(rig_parent_matrix() * Vec4(local, 1.0f));
}

float Application::rig_world_yaw() const {
    if (!has_reference_frame()) return rig_.yaw_rad;
    return rig_.yaw_rad + matrix_yaw(rig_parent_matrix());
}

void Application::set_rig_world_position(const Vec3& world) {
    Vec3 local = world;
    if (has_reference_frame()) {
        local = Vec3(glm::inverse(rig_parent_matrix()) * Vec4(world, 1.0f));
    }
    rig_.pos_x = local.x;
    rig_.pos_y = local.y;
    rig_.pos_z = local.z;
}

void Application::set_rig_world_yaw(float yaw_rad) {
    rig_.yaw_rad = has_reference_frame()
                       ? yaw_rad - matrix_yaw(rig_parent_matrix())
                       : yaw_rad;
}

Vec3 Application::world_vector_to_frame(const Vec3& world_dir) const {
    if (!has_reference_frame()) return world_dir;
    // Rotation only: a direction has no origin to translate.
    const float yaw = matrix_yaw(rig_parent_matrix());
    return glm::angleAxis(-yaw, Vec3(0.0f, 1.0f, 0.0f)) * world_dir;
}

bool Application::has_reference_frame() const {
    return rig_.parent != entt::null && world_.registry().valid(rig_.parent);
}

void Application::set_reference_frame(std::uint32_t entity,
                                      const std::string& anchor) {
    const auto e = static_cast<entt::entity>(entity);
    if (!world_.registry().valid(e)) {
        PTV_WARN("reference frame: entity {} is not valid", entity);
        return;
    }

    // Where the player is in the WORLD right now, before anything changes.
    // Boarding must not move them: a player who steps onto a lift and is
    // instantly somewhere else has been teleported by a bookkeeping detail.
    const Vec3 world_pos(rig_.pos_x, rig_.pos_y, rig_.pos_z);
    const Mat4 before = rig_parent_matrix();
    const Vec3 keep = Vec3(before * Vec4(world_pos, 1.0f));
    const float keep_yaw = rig_.yaw_rad + matrix_yaw(before);

    rig_.parent = e;
    rig_.parent_anchor = anchor;

    // Re-express that same world pose inside the new frame.
    const Mat4 after = rig_parent_matrix();
    const Vec3 local = Vec3(glm::inverse(after) * Vec4(keep, 1.0f));
    rig_.pos_x = local.x;
    rig_.pos_y = local.y;
    rig_.pos_z = local.z;
    rig_.yaw_rad = keep_yaw - matrix_yaw(after);

    PTV_INFO("Player reference frame -> entity {} anchor '{}'", entity,
             anchor.empty() ? "(root)" : anchor.c_str());
}

void Application::clear_reference_frame() {
    if (rig_.parent == entt::null) return;

    // Same argument in reverse: disembarking leaves the player standing where
    // they were, not back at the frame's old origin.
    const Mat4 before = rig_parent_matrix();
    const Vec3 world =
        Vec3(before * Vec4(rig_.pos_x, rig_.pos_y, rig_.pos_z, 1.0f));
    const float world_yaw = rig_.yaw_rad + matrix_yaw(before);

    rig_.parent = entt::null;
    rig_.parent_anchor.clear();
    rig_.pos_x = world.x;
    rig_.pos_y = world.y;
    rig_.pos_z = world.z;
    rig_.yaw_rad = world_yaw;

    PTV_INFO("Player reference frame cleared at ({:.2f}, {:.2f}, {:.2f})",
             world.x, world.y, world.z);
}

void Application::set_player_scale(float scale) {
    const float clamped = std::clamp(scale, kMinPlayerScale, kMaxPlayerScale);
    if (clamped != scale) {
        PTV_WARN("Player scale {:.4f} clamped to {:.4f}", scale, clamped);
    }
    rig_.scale = clamped;
    // The capsule is the player's body, so it has to resize with them or a
    // two-centimetre passenger keeps colliding with a human-sized shape.
    player_capsule_height_ = scene_.player_height_meters * clamped;
}

// ---------------------------------------------------------------------------
// Scene transitions
// ---------------------------------------------------------------------------

std::string Application::save_path_for_scene(const std::string& scene_id) const {
    // Beside the scene document, which is the one directory the runtime is
    // guaranteed to be able to find on both PC VR and inside an APK's
    // extracted data. Per scene, so a temple's puzzle state cannot collide
    // with a laboratory's.
    std::filesystem::path base(scene_path_);
    const std::string name =
        "save." + (scene_id.empty() ? std::string("scene") : scene_id) + ".json";
    return (base.parent_path() / name).string();
}

bool Application::request_scene_transition(const std::string& scene_id,
                                          const std::string& spawn_point_id) {
    if (scene_id.empty()) return false;
    // Only the first request in a frame wins. Two portals touched together
    // would otherwise chain the player through two loads, landing them
    // somewhere neither portal named.
    if (pending_transition_.active) return false;
    if (scene_id == scene_.scene_id) {
        PTV_WARN("transition: already in scene '{}'; ignoring", scene_id);
        return false;
    }
    if (scene_.scene_index.count(scene_id) == 0) {
        // A build-time mistake, not a runtime condition: the scene compiler
        // wrote this index from the same BuildSpec the portal came from, so a
        // miss means they disagree.
        PTV_WARN(
            "transition: scene '{}' is not in this project's scene index; "
            "the portal or menu naming it cannot be taken",
            scene_id);
        return false;
    }
    pending_transition_.active = true;
    pending_transition_.scene_id = scene_id;
    pending_transition_.spawn_point_id = spawn_point_id;
    return true;
}

bool Application::request_scene_restart(bool fresh) {
    if (pending_transition_.active) return false;
    pending_transition_.active = true;
    pending_transition_.restart = true;
    pending_transition_.fresh = fresh;
    pending_transition_.scene_id = scene_.scene_id;
    PTV_INFO("transition: restart of '{}' requested ({})", scene_.scene_id,
             fresh ? "fresh" : "keep state");
    return true;
}

void Application::request_exit() {
    if (exit_requested_) return;
    exit_requested_ = true;
    exit_wait_s_ = 0.0f;
    PTV_INFO("app: exit requested");
}

void Application::apply_pending_transition() {
    if (!pending_transition_.active) return;
    const std::string scene_id = pending_transition_.scene_id;
    const std::string spawn_id = pending_transition_.spawn_point_id;
    const bool restart = pending_transition_.restart;
    const bool fresh = pending_transition_.fresh;
    pending_transition_ = PendingTransition{};

    if (restart) {
        // The attempt counter is the one piece of flow state a restart keeps.
        const int attempts = game_flow_board_.flow_state()->attempts;
        if (!load_scene_at(scene_path_, "", fresh)) {
            PTV_ERROR("restart of '{}' failed; staying put", scene_id);
            return;
        }
        game_flow_board_.flow_state()->attempts = attempts + 1;
        return;
    }

    const auto it = scene_.scene_index.find(scene_id);
    if (it == scene_.scene_index.end()) return;

    // The index stores paths relative to the current document, so a project
    // can be moved or unpacked anywhere and still resolve.
    const std::filesystem::path here =
        std::filesystem::path(scene_path_).parent_path();
    const std::string target = (here / it->second).string();

    if (!load_scene_at(target, spawn_id)) {
        PTV_ERROR("transition to '{}' failed; staying put", scene_id);
    }
}

void Application::place_player_at_spawn(const std::string& spawn_point_id) {
    Vec3 position = scene_.spawn.position;
    Vec3 facing = glm::mat3_cast(scene_.spawn.rotation) * Vec3(0.0f, 0.0f, -1.0f);

    if (!spawn_point_id.empty()) {
        const auto found = std::find_if(
            scene_.spawn_points.begin(), scene_.spawn_points.end(),
            [&](const SpawnPointSpec& s) { return s.id == spawn_point_id; });
        if (found != scene_.spawn_points.end()) {
            position = found->position;
            facing = found->facing;
        } else {
            // Landing at the default is far better than landing nowhere, but
            // it is the wrong door and the player will notice, so say so.
            PTV_WARN(
                "transition: scene '{}' has no spawn point '{}'; using its "
                "default spawn",
                scene_.scene_id, spawn_point_id);
        }
    }

    // A spawn point is authored in the scene's own world space, and the frame
    // the player was riding belonged to the scene being left, so it is already
    // gone by now (load_scene_at clears it). Setting through the world helpers
    // keeps this correct if that ever stops being true.
    set_rig_world_position(position);
    const float fl = glm::length(Vec3(facing.x, 0.0f, facing.z));
    if (fl > 1e-5f) {
        set_rig_world_yaw(yaw_for_facing(facing));
    }
}

bool Application::load_scene_at(const std::string& path,
                                const std::string& spawn_point_id,
                                bool fresh) {
    // Through the asset source, not std::filesystem: on Quest the scene
    // documents live inside the APK, where the filesystem sees nothing.
    if (!asset_source().exists(path)) {
        PTV_ERROR("transition: no scene document at {}", path);
        return false;
    }

    // 1) Remember what the player did to the place they are leaving. Deltas
    //    only -- the outgoing scene is rebuilt from its own document plus
    //    this, so a room the player never touched costs nothing to store.
    //    A fresh restart is a new attempt: nothing is remembered, and what
    //    earlier visits saved is thrown away so no scene restores it later.
    if (fresh) {
        std::error_code ec;
        for (const auto& [id, rel] : scene_.scene_index) {
            (void)rel;
            std::filesystem::remove(save_path_for_scene(id), ec);
        }
        if (!scene_.scene_id.empty()) {
            std::filesystem::remove(save_path_for_scene(scene_.scene_id), ec);
        }
    } else if (!scene_.scene_id.empty()) {
        SaveSystem::save_to_file(save_path_for_scene(scene_.scene_id), world_);
    }

    SceneDoc next{};
    try {
        next = LoadSceneJson(path);
    } catch (const std::exception& e) {
        PTV_ERROR("transition: could not parse {}: {}", path, e.what());
        return false;
    }

    // 2) Tear the old world down. Mechanics first: an instance holds a cached
    //    entity id, and letting it tick once against the new world would drive
    //    whatever entity happens to reuse that id.
    mechanics_.detach_all(world_);
    for (auto& hand : hands_) hand.held = entt::null;
    hand_entities_ = {entt::null, entt::null};
    player_entity_ = entt::null;
    portals_occupied_.clear();
    // The vehicle, lift or NPC the player was riding belongs to the scene
    // being torn down. Left set, the parent handle would dangle -- and entt
    // reuses handles, so it would silently resolve to whatever entity took
    // that slot in the new scene and drag the player around with it.
    rig_.parent = entt::null;
    rig_.parent_anchor.clear();
    // Scale is deliberately NOT reset: a game that shrank the player for a
    // reason should not have that undone by walking through a door.
    // Nested interiors belonged to their hosts, which are gone. Cleared
    // rather than unloaded one by one -- world_.clear() below destroys their
    // entities anyway, and unload_nested_scene would be walking a registry
    // that is about to be emptied.
    nested_scenes_.clear();
    clone_children_.clear();
    encounter_board_.clear();
    game_flow_board_.clear();
    if (projectiles_) projectiles_->clear();
    if (effects_) effects_->clear();
    if (weapons_) weapons_->clear();
    // Physics bodies are dropped by rebuild_from_world below, which tracks
    // them by body id rather than through the world, so it does not matter
    // that the entities are gone by then.
    world_.clear();

    // 3) Stand the new one up, in the same order initialize() does, because
    //    each step depends on the one before: entities before physics bodies,
    //    bodies before the player capsule, the capsule before mechanics that
    //    sweep against it.
    scene_ = std::move(next);
    scene_path_ = path;
    // Signals and live values belong to the scene that raised them.
    if (signal_board_) signal_board_->reset();
    if (var_board_) var_board_->reset();

    place_player_at_spawn(spawn_point_id);
    ApplySceneToWorld(scene_, world_);
    resolve_project_mesh_paths(world_, scene_path_);

    // 4) Overlay what the player already did HERE, before physics reads the
    //    transforms -- a door left open has to be open when its collider is
    //    built, or the player walks into a body that is not where the door is.
    const std::string save = save_path_for_scene(scene_.scene_id);
    if (!fresh && std::filesystem::exists(save)) {
        SaveSystem::load_from_file(save, world_);
    }

    physics_.rebuild_from_world(world_);
    build_nav_graph();
    create_player_body();
    attach_skeletons();
    // world_.clear() destroyed the hands with everything else; without this
    // the player arrived in the next scene with no hands at all.
    if (renderer_ready_) create_hand_entities();
    build_render_env();
    if (renderer_ready_) {
        renderer_.set_performance(scene_.performance);
        governor_.configure(scene_.performance);
        renderer_.build_static_batches(world_, scene_.nav_regions, scene_.nav_links);
    }
    memory_guard_.configure(scene_.performance);
    // The old scene's CPU-side data is gone; give the freed pages back.
    MemoryGuard::purge_heap();
    configure_weapon_systems();
    // The new scene's mechanics declare what they offer as they attach; the
    // player's chosen mode survives the trip if the new scene offers it too.
    mode_available_ = {false, false, false};
    turn_available_ = {false, false};
    input_lock_owners_.clear();
    pending_move_ = Vec3(0.0f);
    pending_jump_mps_ = 0.0f;
    vertical_mode_ = ::ptv::VerticalMode::Grounded;
    vignette_channels_ = {};
    vignette_level_ = 0.0f;
    check_player_mechanic_roles();
    mechanics_.attach_bindings(world_, mech_ctx_);
    // The new scene's NPCs get their own manifest and fresh hit history.
    npc_manifest_logged_ = false;
    npc_last_pos_.clear();
    if (combat_svc_) combat_svc_->clear();
    verify_origin_set_ = false;
    verify_defeats_.clear();

    PTV_INFO("transition: now in scene '{}' ({} entities)", scene_.scene_id,
             static_cast<int>(scene_.entities.size()));
    return true;
}

// ---------------------------------------------------------------------------
// Nested scenes
// ---------------------------------------------------------------------------

bool Application::nested_scene_loaded(const std::string& scene_id) const {
    return std::any_of(nested_scenes_.begin(), nested_scenes_.end(),
                       [&](const NestedScene& n) {
                           return n.scene_id == scene_id;
                       });
}

Mat4 Application::nested_mount_matrix(std::uint32_t host,
                                      const std::string& anchor,
                                      float scale) const {
    Mat4 anchor_world(1.0f);
    const auto e = static_cast<entt::entity>(host);
    if (world_.registry().valid(e)) {
        const AttachAnchor resolved = resolve_attach_anchor(host, anchor);
        if (resolved.is_socket && sockets_svc_ != nullptr) {
            anchor_world = sockets_svc_->socket_world(host, anchor.c_str());
        } else if (resolved.valid && resolved.joint >= 0 &&
                   skeleton_svc_ != nullptr) {
            anchor_world = skeleton_svc_->joint_world(host, resolved.joint);
        } else if (world_.registry().all_of<CTransform>(e)) {
            anchor_world = world_.registry().get<CTransform>(e).t.matrix();
        }
    }
    return glm::scale(anchor_world, Vec3(scale));
}

bool Application::load_nested_scene(const std::string& scene_id,
                                    std::uint32_t host,
                                    const std::string& anchor,
                                    float fit_extent_m) {
    if (scene_id.empty()) return false;
    if (nested_scene_loaded(scene_id)) {
        // Loading twice would stack two skulls' worth of geometry in the same
        // place, and the second copy's ids would collide with the first's.
        PTV_WARN("nested scene '{}' is already loaded", scene_id);
        return false;
    }
    if (!world_.registry().valid(static_cast<entt::entity>(host))) {
        PTV_WARN("nested scene '{}': host entity {} is not valid", scene_id,
                 host);
        return false;
    }

    const auto it = scene_.scene_index.find(scene_id);
    if (it == scene_.scene_index.end()) {
        PTV_WARN(
            "nested scene '{}' is not in this project's scene index; the "
            "mechanic naming it cannot load it",
            scene_id);
        return false;
    }
    const std::filesystem::path here =
        std::filesystem::path(scene_path_).parent_path();
    const std::string path = (here / it->second).string();

    SceneDoc interior{};
    try {
        interior = LoadSceneJson(path);
    } catch (const std::exception& e) {
        PTV_ERROR("nested scene '{}': could not parse {}: {}", scene_id, path,
                  e.what());
        return false;
    }
    if (interior.entities.empty()) {
        PTV_WARN("nested scene '{}' has no entities", scene_id);
        return false;
    }

    // Derive the fit scale from what the interior actually measures. The
    // author was told to work at human scale, so the number is theirs, not a
    // constant we could assume.
    Vec3 lo(std::numeric_limits<float>::max());
    Vec3 hi(std::numeric_limits<float>::lowest());
    for (const auto& es : interior.entities) {
        const Vec3 p = es.transform.position;
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const Vec3 extent = hi - lo;
    const float longest = std::max(extent.x, extent.z);
    float scale = 1.0f;
    if (fit_extent_m > 0.0f && longest > 1e-3f) {
        scale = fit_extent_m / longest;
    }

    SceneNesting nesting{};
    nesting.id_prefix = scene_id;
    nesting.scale = scale;
    nesting.tag = "nested:" + scene_id;
    nesting.mount = nested_mount_matrix(host, anchor, scale);

    ApplySceneToWorld(interior, world_, &nesting);
    resolve_project_mesh_paths(world_, path);

    // Attach every new entity to the host anchor, which is what carries the
    // interior as the host walks and turns. Reusing the attachment system
    // rather than re-transforming the interior each frame means it rides the
    // ANIMATED joint for free, and its colliders are already moved
    // kinematically by tick_attachments.
    // The loader tagged every entity it created, so the tag IS the list of
    // new entities -- no before/after diff of the whole registry needed, and
    // the same tag is what unloading uses later.
    int attached = 0;
    std::vector<entt::entity> fresh;
    {
        auto tagged = world_.registry().view<CTag>();
        for (auto e : tagged) {
            const auto& tags = tagged.get<CTag>(e).tags;
            if (std::find(tags.begin(), tags.end(), nesting.tag) !=
                tags.end()) {
                fresh.push_back(e);
            }
        }
    }
    const Mat4 inv_mount = glm::inverse(nesting.mount);
    for (auto e : fresh) {
        if (!world_.registry().all_of<CTransform>(e)) continue;
        // Already attached inside the interior (a lamp on a moving arm): leave
        // that alone, since its parent is itself carried by the host.
        if (world_.registry().all_of<CAttachment>(e)) continue;
        const auto& t = world_.registry().get<CTransform>(e).t;
        const Mat4 local = inv_mount * t.matrix();
        CAttachment a{};
        a.parent = host;
        a.anchor = anchor;
        a.local_position = Vec3(local[3]);
        a.local_rotation = glm::normalize(glm::quat_cast(Mat4(glm::mat3(
            glm::normalize(Vec3(local[0])), glm::normalize(Vec3(local[1])),
            glm::normalize(Vec3(local[2]))))));
        world_.registry().emplace<CAttachment>(e, std::move(a));
        ++attached;
    }

    physics_.rebuild_from_world(world_);
    mechanics_.attach_bindings(world_, mech_ctx_);

    NestedScene record{};
    record.scene_id = scene_id;
    record.host = host;
    record.anchor = anchor;
    record.scale = scale;
    record.tag = nesting.tag;
    nested_scenes_.push_back(std::move(record));

    PTV_INFO(
        "nested scene '{}' loaded on entity {} anchor '{}': {} entities, "
        "authored {:.1f} m across, scaled {:.4f} to fit {:.2f} m",
        scene_id, host, anchor.empty() ? "(root)" : anchor.c_str(), attached,
        longest, scale, fit_extent_m);
    return true;
}

bool Application::unload_nested_scene(const std::string& scene_id) {
    const auto it = std::find_if(nested_scenes_.begin(), nested_scenes_.end(),
                                 [&](const NestedScene& n) {
                                     return n.scene_id == scene_id;
                                 });
    if (it == nested_scenes_.end()) return false;

    // If the player is inside it, get them out FIRST. Destroying the frame
    // they are parented to would leave the rig pointing at a dead handle,
    // which entt will reuse.
    if (rig_.parent != entt::null &&
        static_cast<std::uint32_t>(rig_.parent) == it->host) {
        clear_reference_frame();
        set_player_scale(1.0f);
    }

    const std::string tag = it->tag;
    std::vector<std::uint32_t> doomed;
    auto view = world_.registry().view<CTag>();
    for (auto e : view) {
        const auto& tags = view.get<CTag>(e).tags;
        if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
            doomed.push_back(static_cast<std::uint32_t>(e));
        }
    }
    for (std::uint32_t e : doomed) destroy_entity_runtime(e);

    nested_scenes_.erase(it);
    PTV_INFO("nested scene '{}' unloaded ({} entities)", scene_id,
             doomed.size());
    return true;
}

void Application::tick_portals(const InputSnapshot& input) {
    if (scene_.portals.empty() || pending_transition_.active) return;

    // The head, not the rig origin: the player's body is where their head is,
    // and a rig origin can sit outside the volume while they lean through it.
    const Vec3 head = player_capsule_center_;

    std::vector<std::string> inside;
    inside.reserve(scene_.portals.size());
    for (const auto& portal : scene_.portals) {
        const Vec3 half = portal.size * 0.5f;
        const Vec3 d = head - portal.position;
        const bool within = std::abs(d.x) <= half.x &&
                            std::abs(d.y) <= half.y &&
                            std::abs(d.z) <= half.z;
        if (!within) continue;
        inside.push_back(portal.id);

        const bool was_inside =
            std::find(portals_occupied_.begin(), portals_occupied_.end(),
                      portal.id) != portals_occupied_.end();

        if (!portal.requires_interaction) {
            // Only on the frame they arrive. Firing every frame they remain
            // would re-request a transition they are already taking.
            if (!was_inside) {
                request_scene_transition(portal.to_scene,
                                         portal.target_spawn_id);
            }
            continue;
        }

        // Gated on a deliberate squeeze, and on the squeeze STARTING while
        // inside -- a player who walked in already holding a grip did not ask
        // to leave.
        for (int hand = 0; hand < 2; ++hand) {
            const auto& h = input.hands[hand];
            if (!h.hand_valid) continue;
            if (h.prev_grip < 0.6f && h.grip >= 0.6f && was_inside) {
                if (haptics_svc_ != nullptr) {
                    haptics_svc_->pulse(hand, 0.06f, 0.5f);
                }
                request_scene_transition(portal.to_scene,
                                         portal.target_spawn_id);
                break;
            }
        }
    }
    portals_occupied_ = std::move(inside);
}

void Application::build_nav_graph() {
    nav_graph_.clear();
    if (scene_.nav_regions.empty()) {
        PTV_INFO("No navigation regions declared; NPCs use local steering");
        return;
    }

    std::unordered_map<std::string, int> by_id;
    for (const auto& r : scene_.nav_regions) {
        nav::NavRegion region{};
        region.id = r.id;
        region.min = r.min;
        region.max = r.max;
        region.floor_y = r.floor_y;
        by_id[r.id] = nav_graph_.add_region(region);
    }

    int dropped = 0;
    for (const auto& l : scene_.nav_links) {
        const auto from = by_id.find(l.from);
        const auto to = by_id.find(l.to);
        if (from == by_id.end() || to == by_id.end()) {
            ++dropped;
            continue;
        }
        nav::NavLink link{};
        link.from = from->second;
        link.to = to->second;
        link.point = l.point;
        link.cost_multiplier = l.cost_multiplier;
        link.bidirectional = l.bidirectional;
        nav_graph_.add_link(link);
    }
    if (dropped > 0) {
        PTV_WARN("Navigation: {} link(s) referenced unknown regions", dropped);
    }
    PTV_INFO("Navigation graph: {} regions, {} links",
             nav_graph_.region_count(), nav_graph_.link_count());
}

void Application::begin_death(std::uint32_t entity) {
    if (!world_.valid(entity) || !world_.has<CHealth>(entity)) return;
    auto& hp = world_.get<CHealth>(entity);
    if (hp.death_timer_s >= 0.0f) return;  // already dying

    hp.current = 0.0f;
    hp.alive = false;

    // Corpses must stop blocking the living. Removing the body rather than
    // leaving it kinematic also means the player can walk over the spot,
    // which is what they will try to do. The player keeps theirs: they are
    // revived in place or respawned, and a capsule-less player walks
    // through walls on the way to the Game Over screen.
    if (entity != static_cast<std::uint32_t>(player_entity_)) {
        physics_.remove_body(entity);
    }

    // Stop it steering: an entity with a locomotion intent would keep
    // walking through its own death animation.
    if (world_.has<CLocomotionIntent>(entity)) {
        world_.remove<CLocomotionIntent>(entity);
    }

    float linger = kDefaultDeathLinger;
    if (skeleton_svc_ != nullptr &&
        skeleton_svc_->play_clip(entity, "die", /*looping=*/false, 0.12f)) {
        const float d = skeleton_svc_->clip_duration(entity);
        if (d > 0.05f) linger = d;
    }
    hp.death_timer_s = linger;
}

void Application::tick_combat(float dt) {
    if (dt <= 0.0f) return;
    std::vector<std::uint32_t> finished;
    {
        std::vector<entt::entity> recovered;
        auto stunned = world_.registry().view<CStun>();
        for (auto e : stunned) {
            auto& s = stunned.get<CStun>(e);
            s.remaining_s -= dt;
            if (s.remaining_s <= 0.0f) recovered.push_back(e);
        }
        for (auto e : recovered) world_.registry().remove<CStun>(e);
    }

    auto view = world_.registry().view<CHealth>();
    for (auto entity : view) {
        auto& hp = view.get<CHealth>(entity);
        if (hp.invuln_s > 0.0f) hp.invuln_s = std::max(0.0f, hp.invuln_s - dt);
        if (hp.death_timer_s < 0.0f) continue;
        hp.death_timer_s -= dt;
        if (hp.death_timer_s <= 0.0f) {
            finished.push_back(static_cast<std::uint32_t>(entity));
        }
    }

    // Destroying inside the view above would invalidate it mid-iteration.
    for (std::uint32_t e : finished) {
        if (e == static_cast<std::uint32_t>(player_entity_)) {
            // The player's death is a game-over decision, not the engine's.
            // Freeze the timer and leave the entity alone; a generated
            // mechanic reads is_alive() and runs whatever restart flow the
            // prompt asked for.
            if (world_.valid(e) && world_.has<CHealth>(e)) {
                world_.get<CHealth>(e).death_timer_s = -1.0f;
            }
            continue;
        }
        destroy_entity_runtime(e);
        if (spawn_svc_) spawn_svc_->forget(e);
    }
}

// ---------------------------------------------------------------------------
// NPC telemetry + verification mode
// ---------------------------------------------------------------------------

namespace {

/** Characters the telemetry reports: anything with a mesh that is an NPC by
 *  tag, faction or locomotion -- never the player, their avatar or hands. */
bool is_npc_entity(const World& w, std::uint32_t e) {
    if (!w.valid(e) || !w.has<CMesh>(e) || !w.has<CTransform>(e)) return false;
    for (const char* t : {"player", "player_avatar", "player_rig", "player_hand"}) {
        if (has_tag(w, e, t)) return false;
    }
    if (w.has<CLocomotionIntent>(e) || w.has<CFaction>(e)) return true;
    for (const char* t : {"npc", "enemy", "ally", "neutral", "critter", "boss"}) {
        if (has_tag(w, e, t)) return true;
    }
    return false;
}

/** "grounded", "flying", "hover:<m>" or "none": which character_locomotion
 *  motor drives this NPC, so verification knows which height check applies. */
std::string npc_motor_of(const World& w, std::uint32_t e) {
    if (!w.has<CMechanicBinding>(e)) return "none";
    for (const auto& entry : w.get<CMechanicBinding>(e).entries) {
        if (entry.id != "builtin.character_locomotion" &&
            entry.builtin_id != "builtin.character_locomotion") {
            continue;
        }
        const auto p = nlohmann::json::parse(entry.params_json, nullptr, false);
        if (!p.is_object()) return "grounded";
        if (p.value("motor", std::string("grounded")) != "flying") return "grounded";
        const float hover = p.value("hover_height_m", 0.0f);
        if (hover > 0.0f) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "hover:%.2f", hover);
            return buf;
        }
        return "flying";
    }
    return "none";
}

std::string fmt_vec(const Vec3& v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f,%.2f,%.2f", v.x, v.y, v.z);
    return buf;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += ',';
        out += p;
    }
    return out.empty() ? "-" : out;
}

}  // namespace

void Application::read_verify_mode() {
    bool on = false;
#if defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ptv.verify", value) > 0) {
        on = value[0] == '1' || value[0] == 't' || value[0] == 'y';
    }
#else
    if (const char* v = std::getenv("PTV_VERIFY")) {
        on = v[0] == '1' || v[0] == 't' || v[0] == 'y';
    }
#endif
    bool flow = false;
#if defined(__ANDROID__)
    char flow_value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ptv.verify_flow", flow_value) > 0) {
        flow = flow_value[0] == '1' || flow_value[0] == 't' || flow_value[0] == 'y';
    }
#else
    if (const char* v = std::getenv("PTV_VERIFY_FLOW")) {
        flow = v[0] == '1' || v[0] == 't' || v[0] == 'y';
    }
#endif
    bool weapons = false;
#if defined(__ANDROID__)
    char weapons_value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ptv.verify_weapons", weapons_value) > 0) {
        weapons = weapons_value[0] == '1' || weapons_value[0] == 't' || weapons_value[0] == 'y';
    }
#else
    if (const char* v = std::getenv("PTV_VERIFY_WEAPONS")) {
        weapons = v[0] == '1' || v[0] == 't' || v[0] == 'y';
    }
#endif
    bool textures = false;
#if defined(__ANDROID__)
    char textures_value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ptv.verify_textures", textures_value) > 0) {
        textures = textures_value[0] == '1' || textures_value[0] == 't' ||
                   textures_value[0] == 'y';
    }
#else
    if (const char* v = std::getenv("PTV_VERIFY_TEXTURES")) {
        textures = v[0] == '1' || v[0] == 't' || v[0] == 'y';
    }
#endif
    verify_textures_ = textures && !flow && !weapons;
    texture_gallery_ = verify_textures_ ? std::make_unique<TextureGallery>() : nullptr;
    if (verify_textures_) {
        PTV_INFO("verify: TEXTURES mode on (each textured asset is shown in front of "
                 "the player in turn for a Quest screenshot)");
    }
    verify_weapons_ = weapons && !flow;
    weapon_verifier_ = verify_weapons_ ? std::make_unique<WeaponVerifier>() : nullptr;
    if (verify_weapons_) {
        PTV_INFO("verify: WEAPONS mode on (player stands still, a scripted hand "
                 "holds and fires every weapon, player cannot die)");
    }
    bool interactions = false;
#if defined(__ANDROID__)
    char interactions_value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.ptv.verify_interactions", interactions_value) > 0) {
        interactions = interactions_value[0] == '1' || interactions_value[0] == 't' ||
                       interactions_value[0] == 'y';
    }
#else
    if (const char* v = std::getenv("PTV_VERIFY_INTERACTIONS")) {
        interactions = v[0] == '1' || v[0] == 't' || v[0] == 'y';
    }
#endif
    verify_interactions_ = interactions && !flow && !verify_weapons_ && !verify_textures_;
    interaction_verifier_ = verify_interactions_
                                ? std::make_unique<InteractionVerifier>(scene_.verify_viewpoints)
                                : nullptr;
    if (verify_interactions_) {
        PTV_INFO("verify: INTERACTIONS mode on (a scripted player settles, walks into, "
                 "grabs, presses and throws every interactive thing; player cannot die)");
    }
    // The flow pass is a verification run too (scripted walk, telemetry).
    verify_flow_ = flow;
    verify_mode_ = on || flow || verify_weapons_ || verify_textures_ || verify_interactions_;
    verify_time_s_ = 0.0f;
    verify_origin_set_ = false;
    if (flow) {
        PTV_INFO("verify: FLOW mode on (scripted walk, player CAN die, end "
                 "screens are clicked by script)");
    } else if (on) {
        PTV_INFO("verify: mode on (scripted player walk, player cannot die)");
    }
}

void Application::tick_verify_autopilot(float dt) {
    if (!verify_mode_ || verify_weapons_ || verify_interactions_ || player_entity_ == entt::null ||
        dt <= 0.0f) {
        return;
    }
    verify_time_s_ += dt;
    // Settle first: spawn, ground the body, let the scene's start delay run.
    constexpr float kSettleS = 3.0f;
    if (verify_time_s_ < kSettleS) return;
    if (!verify_origin_set_) {
        verify_origin_ = rig_world_position();
        verify_origin_set_ = true;
        PTV_INFO("verify: autopilot start at {}", fmt_vec(verify_origin_));
    }
    // A 6 m square, walked one side at a time with a pause after each, so a
    // follower has to start, walk, stop and idle every side, and a chaser
    // has a moving target. Walls are fine: the motor sweeps the body.
    constexpr float kWalkS = 5.0f;
    constexpr float kPauseS = 3.0f;
    constexpr float kSpeed = 1.2f;
    const float t = verify_time_s_ - kSettleS;
    const int seg = static_cast<int>(t / (kWalkS + kPauseS));
    const float in_seg = t - static_cast<float>(seg) * (kWalkS + kPauseS);
    static const Vec3 kDirs[4] = {Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(-1, 0, 0),
                                  Vec3(0, 0, -1)};
    if (in_seg >= kWalkS) return;
    pending_move_ += kDirs[seg % 4] * (kSpeed * dt);
}

void Application::log_npc_manifest() {
    int npcs = 0;
    int templates = 0;
    auto view = world_.registry().view<CMesh, CTransform>();
    for (const auto ent : view) {
        const auto e = static_cast<std::uint32_t>(ent);
        if (!is_npc_entity(world_, e)) continue;
        const bool tmpl = has_tag(world_, e, "template");
        (tmpl ? templates : npcs) += 1;
        const auto& cm = view.get<CMesh>(ent);
        const std::string* name = world_.id_of(ent);
        std::vector<std::string> clips;
        const auto* anim = world_.registry().try_get<CAnimator>(ent);
        for (const auto& [intent, clip] : cm.animations) {
            bool found = false;
            if (anim != nullptr) {
                for (const auto& c : anim->clips) {
                    if (c.name == clip) {
                        found = true;
                        break;
                    }
                }
            }
            clips.push_back(intent + (found ? ":ok" : ":missing"));
        }
        // A template's mechanics attach to each clone, so list what is bound
        // rather than what is attached (nothing, by design).
        std::vector<std::string> bound;
        if (const auto* mb = world_.registry().try_get<CMechanicBinding>(ent)) {
            for (const auto& b : mb->entries) {
                bound.push_back(b.builtin_id.empty() ? b.id : b.builtin_id);
            }
        }
        std::vector<std::string> unresolved;
        for (const auto& id : bound) {
            if (mechanics_.unresolved().count(id) != 0) unresolved.push_back(id);
        }
        PTV_INFO(
            "npc_manifest id={} e={} asset={} template={} skeleton={} skinned={} "
            "clips={} mechanics={} attached={} unresolved={} pos={}",
            name ? *name : std::string("?"), e, cm.asset_id, tmpl ? 1 : 0,
            world_.registry().all_of<CSkeleton>(ent) ? 1 : 0, cm.skinned ? 1 : 0,
            join(clips), join(bound), join(mechanics_.attached_ids(e)),
            join(unresolved), fmt_vec(view.get<CTransform>(ent).t.position));
    }
    std::vector<std::string> unresolved_all;
    for (const auto& [id, n] : mechanics_.unresolved()) {
        unresolved_all.push_back(id + "x" + std::to_string(n));
    }
    PTV_INFO("npc_manifest_summary scene={} npcs={} templates={} unresolved={} "
             "player={} verify={}",
             scene_.scene_id, npcs, templates, join(unresolved_all),
             player_entity_ == entt::null
                 ? std::string("none")
                 : std::to_string(static_cast<std::uint32_t>(player_entity_)),
             verify_mode_ ? 1 : 0);
}

void Application::watch_npc_heights(const char* phase) {
    // Startup only: an NPC that jumps more than a metre in one step of the
    // frame names the step that moved it. Cheap (a few dozen entities, the
    // first 600 frames), and it is what tells a floor-probe climb from a
    // physics or attachment write.
    if (frame_index_ > 600 || npc_watch_warnings_ >= 12) return;
    for (auto e : world_.registry().view<CTransform, CMesh>()) {
        const auto id = static_cast<std::uint32_t>(e);
        if (!is_npc_entity(world_, id) || has_tag(world_, id, "template")) continue;
        const float y = world_.get<CTransform>(id).t.position.y;
        auto it = npc_watch_y_.find(id);
        if (it != npc_watch_y_.end() && std::fabs(y - it->second) > 1.0f &&
            npc_watch_warnings_ < 12) {
            ++npc_watch_warnings_;
            const std::string* name = world_.id_of(e);
            PTV_WARN("npc_jump: '{}' y {:.2f} -> {:.2f} at {} (frame {})",
                     name != nullptr ? *name : std::to_string(id), it->second, y, phase,
                     frame_index_);
        }
        npc_watch_y_[id] = y;
    }
}

void Application::tick_npc_telemetry(float dt) {
    if (dt <= 0.0f) return;
    npc_telemetry_clock_s_ += dt;
    if (!verify_defeats_.empty() && combat_svc_) {
        for (auto it = verify_defeats_.begin(); it != verify_defeats_.end();) {
            it->second -= dt;
            if (it->second > 0.0f) {
                ++it;
                continue;
            }
            if (world_.valid(it->first) && combat_svc_->is_alive(it->first)) {
                combat_svc_->kill(it->first);
                PTV_INFO("verify: defeated entity {} after it hit the player",
                         it->first);
            }
            it = verify_defeats_.erase(it);
        }
    }
    if (!npc_manifest_logged_) {
        npc_manifest_logged_ = true;
        log_npc_manifest();
    }
    npc_telemetry_accum_s_ += dt;
    if (npc_telemetry_accum_s_ < 1.0f) return;
    const float window = npc_telemetry_accum_s_;
    npc_telemetry_accum_s_ = 0.0f;

    const Vec3 player = player_capsule_center_;
    constexpr int kMaxLines = 16;
    int lines = 0;
    int live = 0;
    auto view = world_.registry().view<CMesh, CTransform>();
    for (const auto ent : view) {
        const auto e = static_cast<std::uint32_t>(ent);
        if (!is_npc_entity(world_, e) || has_tag(world_, e, "template")) continue;
        ++live;
        if (lines >= kMaxLines) continue;
        ++lines;
        const auto& cm = view.get<CMesh>(ent);
        const Vec3 pos = view.get<CTransform>(ent).t.position;
        float speed = 0.0f;
        float vertical_speed = 0.0f;
        if (auto it = npc_last_pos_.find(e); it != npc_last_pos_.end()) {
            const Vec3 d(pos.x - it->second.x, 0.0f, pos.z - it->second.z);
            speed = glm::length(d) / window;
            vertical_speed = (pos.y - it->second.y) / window;
        }
        npc_last_pos_[e] = pos;
        // Height of the soles above the FLOOR below -- level geometry only.
        // Measured against anything else (the rifle in its own hands) this
        // read a steady -0.63 m while the NPC was climbing into the sky.
        const float h = cm.target_scale_meters > 1.0e-4f ? cm.target_scale_meters
                                                         : 1.8f;
        const float foot = cm.foot_offset_m > 1.0e-4f ? cm.foot_offset_m : 0.5f * h;
        PhysicsWorld::RayFilter filter{};
        filter.ignore.push_back(e);
        filter.floor_only = true;
        Vec3 hit(0.0f), n(0.0f, 1.0f, 0.0f);
        std::uint32_t what = InputSnapshot::kInvalidEntity;
        std::string feet_gap = "none";
        std::string origin_gap = "none";
        // Just above the soles first (under a table top or a balcony), then
        // from the head for a body sunk below its floor.
        const bool floor_hit =
            physics_.raycast(Vec3(pos.x, pos.y - foot + 0.5f, pos.z), Vec3(0, -1, 0),
                             200.0f, filter, hit, n, what) ||
            physics_.raycast(pos + Vec3(0.0f, 0.5f * h, 0.0f), Vec3(0, -1, 0),
                             200.0f, filter, hit, n, what);
        if (floor_hit) {
            char buf[32];
            // What the CONTRACT says: origin minus the declared foot offset.
            std::snprintf(buf, sizeof(buf), "%.2f", pos.y - foot - hit.y);
            origin_gap = buf;
            // What is actually DRAWN: the lowest animated foot/toe joint,
            // less its bind-pose height above the mesh's real soles. This is
            // independent of foot_offset_m, so a model whose drawn feet do
            // not match its capsule shows up here even when the contract
            // looks perfect.
            float sole = pos.y - foot;
            if (skeleton_svc_ != nullptr &&
                world_.registry().all_of<CSkeleton>(ent)) {
                const auto& sk = world_.registry().get<CSkeleton>(ent);
                const float s = view.get<CTransform>(ent).t.scale;
                float best = 1.0e30f;
                for (const char* slot : {"foot_l", "foot_r", "toes_l", "toes_r"}) {
                    const int j = skeleton_svc_->joint_for_slot(e, slot);
                    if (j < 0) continue;
                    const float above_sole =
                        skeleton_svc_->joint_bind_model(e, j)[3].y -
                        sk.skeleton.bind_min_y;
                    best = std::min(best, skeleton_svc_->joint_world(e, j)[3].y -
                                              above_sole * s);
                }
                if (best < 1.0e29f) sole = best;
            }
            std::snprintf(buf, sizeof(buf), "%.2f", sole - hit.y);
            feet_gap = buf;
        }
        // Angle between where the body faces and where it is meant to face:
        // its travel direction, or the target it is tracking.
        std::string facing_err = "-";
        if (const auto* li = world_.registry().try_get<CLocomotionIntent>(ent)) {
            Vec3 want(0.0f);
            if (li->has_face_target && !li->face_travel) {
                want = li->face_target - pos;
            } else {
                want = li->desired_velocity;
            }
            want.y = 0.0f;
            if (glm::length(want) > 0.2f) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.0f",
                              glm::degrees(heading_error(
                                  facing_of(view.get<CTransform>(ent).t.rotation),
                                  want)));
                facing_err = buf;
            }
        }
        std::string clip = "-";
        if (const auto* anim = world_.registry().try_get<CAnimator>(ent)) {
            if (anim->active_clip >= 0 &&
                anim->active_clip < static_cast<int>(anim->clips.size())) {
                clip = anim->clips[static_cast<std::size_t>(anim->active_clip)].name;
            }
        }
        float intent = 0.0f;
        if (const auto* li = world_.registry().try_get<CLocomotionIntent>(ent)) {
            intent = glm::length(Vec3(li->desired_velocity.x, 0.0f,
                                      li->desired_velocity.z));
        }
        const bool alive = !world_.registry().all_of<CHealth>(ent) ||
                           world_.registry().get<CHealth>(ent).alive;
        const Vec3 to_player(player.x - pos.x, 0.0f, player.z - pos.z);
        const std::string* name = world_.id_of(ent);
        std::string tags_csv;
        if (const auto* tg = world_.registry().try_get<CTag>(ent)) {
            for (const auto& t : tg->tags) {
                if (!tags_csv.empty()) tags_csv += ',';
                tags_csv += t;
            }
        }
        PTV_INFO(
            "npc t={:.1f} id={} e={} asset={} pos={} feet_gap={} "
            "origin_gap={} vy={:.2f} facing_err_deg={} motor={} speed={:.2f} "
            "intent={:.2f} clip=\"{}\" alive={} dist_player={:.2f} tags={}",
            npc_telemetry_clock_s_, name ? *name : std::string("clone"), e,
            cm.asset_id, fmt_vec(pos), feet_gap, origin_gap, vertical_speed, facing_err,
            npc_motor_of(world_, e), speed, intent, clip, alive ? 1 : 0,
            glm::length(to_player), tags_csv.empty() ? std::string("-") : tags_csv);
    }
    // Forget NPCs that no longer exist so a reused entity id starts clean.
    for (auto it = npc_last_pos_.begin(); it != npc_last_pos_.end();) {
        if (!world_.valid(it->first)) {
            it = npc_last_pos_.erase(it);
        } else {
            ++it;
        }
    }
    PTV_INFO(
        "npc_summary t={:.1f} live={} encounter_phase={} alive={} wave={}/{} "
        "defeated={} player_hits={} player_pos={}",
        npc_telemetry_clock_s_, live, static_cast<int>(encounter_board_.phase()),
        encounter_board_.alive(), encounter_board_.wave_index() + 1,
        encounter_board_.wave_count(), encounter_board_.defeated_total(),
        combat_svc_ ? combat_svc_->player_hits() : 0, fmt_vec(player));
}

entt::entity Application::clone_entity(std::uint32_t template_entity,
                                       const Vec3& position,
                                       const Quat& rotation) {
    const auto src = static_cast<entt::entity>(template_entity);
    // The instance and every piece of gear attached to it (a club in its
    // hand, a lantern) -- see ptv/ecs/Clone.h for what is copied and why.
    std::vector<CloneRecord> made;
    const auto dst = clone_entity_tree(world_, src, position, rotation, made);
    if (dst == entt::null) {
        PTV_WARN("spawn: template entity {} is not a valid spawnable",
                 template_entity);
        return entt::null;
    }
    // Physics bodies and mechanics only once the whole tree exists, so an
    // instance's mechanic that looks for its gear on attach finds it.
    for (const auto& rec : made) {
        const auto id = static_cast<std::uint32_t>(rec.entity);
        attach_entity_runtime(id);
        if (rec.gear) clone_children_.insert(id);
    }
    return dst;
}

void Application::attach_entity_runtime(std::uint32_t entity) {
    if (!world_.valid(entity)) return;
    physics_.add_body_for(world_, entity);
    mechanics_.attach_entity(world_, entity, mech_ctx_);
}

void Application::destroy_entity_runtime(std::uint32_t entity) {
    if (!world_.valid(entity)) return;
    if (entity == static_cast<std::uint32_t>(player_entity_)) {
        PTV_WARN("spawn: refusing to despawn the player");
        return;
    }
    // Gear that was cloned onto this instance goes with it; gear the scene
    // authored on its own stays (it just stops following).
    if (!clone_children_.empty()) {
        std::vector<std::uint32_t> children;
        auto view = world_.registry().view<CAttachment>();
        for (const auto child : view) {
            if (view.get<CAttachment>(child).parent != entity) continue;
            const auto id = static_cast<std::uint32_t>(child);
            if (clone_children_.count(id) != 0) children.push_back(id);
        }
        for (const auto c : children) {
            clone_children_.erase(c);
            destroy_entity_runtime(c);
        }
    }
    mechanics_.detach_entity(world_, entity);
    physics_.remove_body(entity);
    // Drop anything still holding it, or grab would keep driving a dead
    // entity's transform every frame.
    for (auto& hand : hands_) {
        if (hand.held == static_cast<entt::entity>(entity)) {
            hand.held = entt::null;
        }
    }
    world_.registry().destroy(static_cast<entt::entity>(entity));
}

void Application::attach_skeletons() {
    int rigged = 0;
    std::unordered_map<std::string, gltf_mesh_load::ClipList> shared_clips;
    auto view = world_.registry().view<CMesh>();
    for (auto entity : view) {
        const auto& cm = view.get<CMesh>(entity);
        if (cm.glb_path.empty()) continue;

        // scene.json already says which assets are rigged (the optimizer
        // reported it), so the common case costs nothing. Fall back to
        // probing the GLB's JSON chunk for hand-authored scenes and samples
        // that predate the flag -- still no BIN decoding.
        if (!cm.skinned) {
            bool has_skin = false;
            int probe_joints = 0;
            std::string probe_err;
            if (!gltf_mesh_load::glb_probe_skin(cm.glb_path, &has_skin,
                                                &probe_joints, probe_err)) {
                PTV_WARN("skin probe failed for {}: {}", cm.glb_path,
                         probe_err);
                continue;
            }
            if (!has_skin) continue;
        }

        gltf_mesh_load::ModelFrame frame{};
        if (cm.has_model_frame) {
            frame.valid = true;
            frame.yaw_offset_rad = cm.model_yaw_offset_rad;
            frame.sole_y = cm.model_sole_y;
            frame.height = cm.model_height;
            frame.unit_scale = cm.unit_scale;
        } else {
            PTV_WARN(
                "rigged mesh {} has no model frame in scene.json; it is scaled "
                "by its longest axis and faces whatever its source faced. "
                "Re-run asset_contract and scene_compile.",
                cm.glb_path);
        }
        const gltf_mesh_load::ModelFrame* frame_ptr =
            frame.valid ? &frame : nullptr;

        gltf_mesh_load::SkeletonData skel;
        const int slot = renderer_.load_glb_skinned(
            cm.glb_path, cm.target_scale_meters, skel, frame_ptr);
        if (slot < 0 || skel.joint_count() == 0) {
            PTV_WARN("rigged mesh {} failed to load; drawing nothing for it",
                     cm.glb_path);
            continue;
        }
        if (cm.foot_offset_m <= 1.0e-4f) {
            // No measured offset: stand it on the soles the loaded bind pose
            // actually has. Half the height would float a feet-origin model
            // by half its height, which is exactly what Tripo NPCs did.
            auto& mutable_cm = view.get<CMesh>(entity);
            mutable_cm.foot_offset_m = std::max(-skel.bind_min_y, 1.0e-3f);
            PTV_WARN(
                "rigged mesh {} has no foot_offset_m; measured {:.3f} m from "
                "its bind pose",
                cm.glb_path, mutable_cm.foot_offset_m);
        }

        // Every LOD shares the skeleton by construction (ptv_glb_optimize
        // copies it verbatim), so the coarser levels only need their geometry
        // resident for the renderer's distance switch to work.
        for (size_t i = 1; i < cm.lod_paths.size(); ++i) {
            gltf_mesh_load::SkeletonData ignored;
            renderer_.load_glb_skinned(cm.lod_paths[i], cm.target_scale_meters,
                                       ignored, frame_ptr);
        }

        // One clip array per rigged asset AND load: the loader bakes the
        // scale and model-frame pivot into the root joint's channels, so only
        // entities loaded identically may share (runtime clones copy their
        // source's skeleton, so they share through the copy).
        const std::string clip_key = fmt::format(
            "{}|{:.6f}|{:d}|{:.6f}|{:.6f}|{:.6f}|{:.8f}", cm.glb_path, cm.target_scale_meters,
            frame.valid ? 1 : 0, frame.yaw_offset_rad, frame.sole_y, frame.height,
            frame.unit_scale);
        if (auto shared = shared_clips.find(clip_key); shared != shared_clips.end()) {
            skel.clips = shared->second;
        } else {
            shared_clips.emplace(clip_key, skel.clips);
        }

        CSkeleton cs{};
        cs.local_pose = skel.bind_pose;
        // Sockets on a rigged mesh ride the joint nearest them, so a muzzle
        // on a goblin's built-in rifle follows the animated arm.
        cs.socket_joint = bind_sockets_to_joints(cm.sockets, skel);
        cs.skeleton = std::move(skel);
        gltf_mesh_load::build_joint_palette(cs.skeleton, cs.local_pose,
                                            cs.palette);

        CAnimator anim{};
        anim.clips = cs.skeleton.clips;
        anim.joint_overridden.assign(cs.skeleton.joint_count(), 0u);
        anim.active_clip = -1;

        // Start an idle loop when the asset has one. Which clip to play IS a
        // gameplay decision in general, but "stand still" is the one case
        // with an obvious answer, and without it every enemy in the scene
        // holds its bind pose until some mechanic thinks to start it -- a
        // room full of T-posing goblins reads as broken, not as idle.
        const auto idle = cm.animations.find("idle");
        if (idle != cm.animations.end()) {
            for (size_t i = 0; i < anim.clips.size(); ++i) {
                if (anim.clips[i].name != idle->second) continue;
                anim.active_clip = static_cast<int>(i);
                anim.looping = true;
                anim.playing = true;
                break;
            }
        }

        world_.registry().emplace_or_replace<CSkeleton>(entity, std::move(cs));
        world_.registry().emplace_or_replace<CAnimator>(entity,
                                                        std::move(anim));
        ++rigged;
    }
    if (rigged > 0) {
        PTV_INFO("Attached {} skeleton(s) from rigged GLBs", rigged);
    }
    // Gear authored to sit on a hand by its own grip socket (an NPC's
    // rifle): seat it now that the hand's rig is known.
    if (attach_svc_ != nullptr) {
        std::vector<std::pair<std::uint32_t, CAttachment>> pending;
        auto av = world_.registry().view<CAttachment>();
        for (const auto e : av) {
            const auto& a = av.get<CAttachment>(e);
            if (!a.child_socket.empty()) pending.emplace_back(static_cast<std::uint32_t>(e), a);
        }
        for (const auto& [child, a] : pending) {
            if (!attach_svc_->attach_by_socket(child, a.child_socket.c_str(), a.parent,
                                               a.anchor.c_str())) {
                PTV_WARN("scene: could not seat entity {} by socket '{}' on '{}'", child,
                         a.child_socket, a.anchor);
            }
            if (world_.registry().all_of<CAttachment>(static_cast<entt::entity>(child))) {
                world_.registry().get<CAttachment>(static_cast<entt::entity>(child)).child_socket.clear();
            }
        }
    }
}

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
        uptime_s_ += dt;

        // Leave Game: ask the runtime to end the session once; poll_events
        // reports EXITING and the loop ends. A runtime that never answers
        // (no session, a desktop run without a headset) is not waited on
        // forever.
        if (exit_requested_) {
            if (!exit_sent_) {
                exit_sent_ = true;
                if (!xr_.request_exit()) running_ = false;
            }
            exit_wait_s_ += dt;
            if (exit_wait_s_ > 3.0f) {
                PTV_WARN("app: runtime did not end the session; exiting anyway");
                running_ = false;
            }
        }

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

        // The rig transforms the stage origin into the world. We need
        // this to compute world-space hand poses for grab detection and
        // for the per-frame kinematic follow.
        // Composed in one place (rig_world_matrix) so the reference frame and
        // player scale reach every consumer at once. A second composition
        // anywhere would drift from this one, and the player's hands would
        // stop matching their eyes.
        const Mat4 rig_mat = rig_world_matrix();

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
        bool hand_valid[2] = {input.hand_left_valid, input.hand_right_valid};
        // The interactions verification pass drives the hands itself: its
        // pose replaces the tracked one here, at the source, so the drawn
        // hand, its fingertip, its colliders and its velocity all follow.
        bool scripted[2] = {false, false};
        Vec3 scripted_pos[2]{Vec3(0.0f), Vec3(0.0f)};
        Quat scripted_rot[2]{Quat(1, 0, 0, 0), Quat(1, 0, 0, 0)};
        if (interaction_verifier_) {
            for (int h = 0; h < 2; ++h) {
                bool valid = false;
                if (interaction_verifier_->scripted_pose(h, &scripted_pos[h], &scripted_rot[h],
                                                         &valid)) {
                    scripted[h] = true;
                    hand_valid[h] = valid;
                }
            }
        }
        const XrPosef hand_poses[2] = {input.hand_left.pose, input.hand_right.pose};
        const bool aim_valid[2]  = {input.aim_left_valid, input.aim_right_valid};
        const XrPosef aim_poses[2] = {input.aim_left.pose, input.aim_right.pose};
        // Total heading, including the parent frame's. Reading rig_.yaw_rad
        // alone was correct only while the rig was always world-space: a
        // player riding a turning vehicle would otherwise have hands that
        // rotate with the vehicle while their reported orientation does not.
        // Still yaw-only, because the frame itself is (see
        // rig_parent_matrix).
        const Quat rig_yaw_quat =
            glm::angleAxis(matrix_yaw(rig_mat), Vec3(0, 1, 0));
        Mat4 hand_world[2]{Mat4(1.0f), Mat4(1.0f)};
        Vec3 hand_world_pos[2]{Vec3(0.0f), Vec3(0.0f)};
        Quat hand_world_rot[2]{Quat(1,0,0,0), Quat(1,0,0,0)};
        Quat aim_world_rot[2]{Quat(1,0,0,0), Quat(1,0,0,0)};
        bool aim_world_valid[2]{false, false};
        for (int h = 0; h < 2; ++h) {
            if (scripted[h]) {
                if (hand_valid[h]) {
                    hand_world[h] = glm::translate(Mat4(1.0f), scripted_pos[h]) *
                                    glm::mat4_cast(scripted_rot[h]);
                    hand_world_pos[h] = scripted_pos[h];
                    hand_world_rot[h] = scripted_rot[h];
                    aim_world_rot[h] = scripted_rot[h];
                    aim_world_valid[h] = true;
                }
            } else if (hand_valid[h]) {
                hand_world[h] = rig_mat * xr_pose_to_matrix(hand_poses[h]);
                hand_world_pos[h] = mat_translation(hand_world[h]);
                // Rig rotation is yaw only, so we can compose rotations directly.
                hand_world_rot[h] = rig_yaw_quat * xr_pose_to_quat(hand_poses[h]);
            }
            if (scripted[h]) {
                // Pose set above.
            } else if (aim_valid[h]) {
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

            // Keep the rigged hand entity on the controller. Writing
            // CTransform here rather than at draw time means grab, sockets
            // and any mechanic that reads the hand entity all agree with what
            // is on screen. Position/rotation only -- the pose of the FINGERS
            // is builtin.hand_pose's business.
            const auto he = hand_entities_[static_cast<size_t>(h)];
            if (hand_valid[h]) last_grip_rot_[static_cast<size_t>(h)] = hand_world_rot[h];
            if (he != entt::null && hand_valid[h] &&
                world_.registry().all_of<CTransform>(he)) {
                place_hand_entity(h, hand_world_pos[h], hand_world_rot[h]);
            } else if (he != entt::null && verify_mode_ && !weapon_verifier_ &&
                       !interaction_verifier_ && world_.registry().all_of<CTransform>(he)) {
                // Verification on a headset lying on a desk: no controller is
                // tracked, so hold the visible hands where a player's would
                // be (in front of the eyes) so the run can confirm they draw.
                // Only the visual moves; nothing reads this as input.
                const Vec3 fwd = glm::length(last_head_forward_xz_) > 1e-3f
                                     ? glm::normalize(last_head_forward_xz_)
                                     : Vec3(0.0f, 0.0f, -1.0f);
                const Vec3 right = glm::normalize(glm::cross(fwd, Vec3(0.0f, 1.0f, 0.0f)));
                const Vec3 at = last_head_world_pos_ + fwd * 0.38f - Vec3(0.0f, 0.25f, 0.0f) +
                                right * (h == 0 ? -0.18f : 0.18f);
                place_hand_entity(h, at, glm::quatLookAtRH(fwd, Vec3(0.0f, 1.0f, 0.0f)));
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
        // The eyes always render (and cull) from the real headset.
        const XrPosef real_head_pose = input.head.pose;
        const bool real_head_valid = input.head_valid;
        // The interactions pass drives the head too: a standing player, eyes
        // kEyeHeightM over its feet, level, facing the way the pass turned
        // the rig. A headset lying on a desk otherwise sized the full-body
        // avatar to 0.75 and turned its torso away from the scripted hand.
        if (interaction_verifier_ && interaction_verifier_->scripts_head()) {
            input.head_valid = true;
            input.head.pose.position = {0.0f, InteractionVerifier::kEyeHeightM, 0.0f};
            input.head.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }
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

        // The headset pose in stage space, kept so the player motor, teleport
        // and snap turn can recompose the head against a rig that moved
        // earlier this frame.
        if (input.head_valid) {
            head_local_ = xr_pose_to_matrix(input.head.pose);
            head_local_valid_ = true;
        }

        // Without the motor (riding a vehicle, a shrunk player) the contact
        // capsule simply follows the headset, as it always did. With it, the
        // motor owns the capsule and moves it after locomotion has run.
        if (!motor_active()) update_player_body(head_world_pos, dt);

        // A game whose default locomotion mode is not one it binds would
        // otherwise leave the stick doing nothing.
        reconcile_available_modes();

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
        // How far the rig moved since the poses above were composed. Identity
        // until the motor runs; afterwards it carries every tracked pose along
        // with the rig, so hands and head reach Simulate-phase mechanics (the
        // avatar's arm IK above all) where they are NOW, not a stride behind.
        Mat4 pose_correction(1.0f);
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
                hs.thumbstick_touched = input.thumbstick_touch[h];
                hs.prev_thumbstick_touched = prev_input_.thumbstick_touch[h];
                hs.thumbstick_click = input.thumbstick_click[h];
                hs.prev_thumbstick_click = prev_input_.thumbstick_click[h];
                hs.face_button_touched =
                    (h == 0) ? (input.button_x_touch || input.button_y_touch)
                             : (input.button_a_touch || input.button_b_touch);
                hs.prev_face_button_touched =
                    (h == 0) ? (prev_input_.button_x_touch ||
                                prev_input_.button_y_touch)
                             : (prev_input_.button_a_touch ||
                                prev_input_.button_b_touch);
                if (hand_valid[h]) {
                    hs.hand_position = hand_world_pos[h];
                    hs.hand_rotation = hand_world_rot[h];
                    hs.hand_valid = true;
                }
                if (aim_world_valid[h]) {
                    hs.aim_position = mat_translation(
                        pose_correction * rig_mat *
                        xr_pose_to_matrix(aim_poses[h]));
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
            s.button_menu = input.button_menu;
            s.prev_button_menu = prev_input_.button_menu;
            if (input.head_valid) {
                s.head_position = head_world_pos;
                s.head_rotation = head_world_rot;
                s.head_valid = true;
            }
            if (weapon_verifier_) weapon_verifier_->patch(s);
            if (interaction_verifier_) interaction_verifier_->patch(s);
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
        ++frame_index_;
        if (contacts_svc_) contacts_svc_->begin_frame();
        {
            const InputSnapshot provisional = build_snapshot();
            mechanics_.update(world_, dt, provisional, mech_ctx_,
                              MechanicPhase::Input);
        }
        tick_verify_autopilot(dt);

        // ------------------------------------------------------------
        // Player motor.
        //
        // Locomotion mechanics only REQUESTED movement during the Input
        // phase. Resolve it now, together with the player's physical
        // walking, against the world: stick movement is swept through the
        // body so walls stop it from every direction, the head is kept out
        // of walls, and the feet are put back on the ground.
        // ------------------------------------------------------------
        if (motor_active()) {
            step_player_motor(dt);
        } else {
            apply_pending_move_legacy();
        }
        {
            const Mat4 rig_after = rig_world_matrix();
            pose_correction = rig_after * glm::inverse(rig_mat);
            const Quat corr_rot = glm::angleAxis(
                matrix_yaw(rig_after) - matrix_yaw(rig_mat), Vec3(0, 1, 0));
            for (int h = 0; h < 2; ++h) {
                hand_world[h] = pose_correction * hand_world[h];
                hand_world_pos[h] =
                    Vec3(pose_correction * Vec4(hand_world_pos[h], 1.0f));
                hand_world_rot[h] = corr_rot * hand_world_rot[h];
                aim_world_rot[h] = corr_rot * aim_world_rot[h];
                if (hand_valid[h]) last_grip_rot_[static_cast<size_t>(h)] = hand_world_rot[h];
                // A hand anchored to what it holds (IHands::set_hand_anchor,
                // set by builtin.grab during the Input phase) is drawn on
                // the item's socket. The anchor was computed before the
                // motor's correction, so it gets the same correction, and
                // is written back for Simulate-phase readers (the avatar).
                Vec3 place_pos = hand_world_pos[h];
                Quat place_rot = hand_world_rot[h];
                auto& anchor = hand_anchors_[static_cast<size_t>(h)];
                if (anchor.frame == frame_index_ && anchor.weight > 0.0f) {
                    anchor.position = Vec3(pose_correction * Vec4(anchor.position, 1.0f));
                    anchor.rotation = glm::normalize(corr_rot * anchor.rotation);
                    place_pos = glm::mix(place_pos, anchor.position, anchor.weight);
                    place_rot = glm::normalize(
                        glm::slerp(place_rot, anchor.rotation, anchor.weight));
                }
                const auto he = hand_entities_[static_cast<size_t>(h)];
                if (he != entt::null && hand_valid[h] &&
                    world_.registry().all_of<CTransform>(he)) {
                    place_hand_entity(h, place_pos, place_rot);
                }
            }
            if (input.head_valid) {
                head_world_pos =
                    Vec3(pose_correction * Vec4(head_world_pos, 1.0f));
                head_world_rot = corr_rot * head_world_rot;
                last_head_world_pos_ = head_world_pos;
            }
            draw_hand_debug_axes(hand_world_pos, hand_world_rot, hand_valid);
        }

        // Left-stick SLIDE locomotion now lives in the built-in mechanics
        // builtin.locomotion.slide.head_relative and
        // builtin.locomotion.slide.controller_relative (Input phase). They
        // gate themselves on ILocomotion::active_mode() and input_locked(),
        // so an open settings menu still suppresses movement.

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
        watch_npc_heights("before physics");
        physics_.step(dt);
        physics_.sync_to_world(world_);
        watch_npc_heights("physics sync");

        // Bodies just moved, so the overlap sets computed during the Input
        // phase describe where things used to be. Drop them; Simulate-phase
        // queries recompute against settled positions.
        if (contacts_svc_) contacts_svc_->begin_frame();

        mechanics_.update(world_, dt, snapshot, mech_ctx_,
                          MechanicPhase::Simulate);
        watch_npc_heights("simulate mechanics");
        if (weapon_verifier_) {
            weapon_verifier_->tick(dt, world_, mech_ctx_, head_world_pos, head_world_rot,
                                   scene_.performance.max_live_projectiles);
        }
        if (interaction_verifier_) {
            interaction_verifier_->tick(dt, world_, mech_ctx_, head_world_pos, head_world_rot);
        }

        // Every motion mechanic has asked for (or not asked for) a vignette
        // this frame; turn that into what the renderer draws.
        tick_comfort_vignette(dt);

        // Decay the purely visual transform offsets (recoil kick, flinch)
        // AFTER Simulate so an impulse requested this frame is drawn this
        // frame, and before the draw list is built below.
        tick_transform_offsets(world_, dt);

        // Projectiles fired this frame start moving now; then every effect
        // (including the projectiles' own trails) becomes this frame's quads.
        if (projectiles_) projectiles_->tick(dt);
        if (effects_) effects_->tick(dt, renderer_);

        // Evaluate skeletal poses in the same window and for the same reason:
        // a mechanic that curled a finger from the trigger during Simulate
        // has already written its override, so sampling clips and rebuilding
        // joint palettes here means the pose the renderer picks up below is
        // this frame's, not last frame's.
        {
            // Distance throttling for pose evaluation. Measured from the
            // headset, because that is what the player is looking from --
            // an NPC 30 m away costs the same to pose as one in your face,
            // and posing thirty of them every frame is what pushes a Quest
            // off its frame budget long before triangles do.
            AnimationLod lod{};
            const auto& bands = scene_.performance.animation_lod_distances_m;
            if (bands.size() >= 3) {
                lod.full_rate_m = bands[0];
                lod.half_rate_m = bands[1];
                lod.stop_m = bands[2];
            }
            tick_animators(world_, dt, last_head_world_pos_, lod);
        }

        // Attachments come last, after the poses they follow exist: a rifle
        // parented to a goblin's hand has to read the hand's ANIMATED
        // position, not the bind pose it had at the top of the frame.
        tick_attachments(dt);
        // The player's hands and limbs as colliders, on this frame's pose.
        if (player_parts_ && skeleton_svc_ && hands_svc_) {
            player_parts_->update(world_, physics_, *skeleton_svc_, *hands_svc_, snapshot, dt);
        }

        // Age invulnerability windows and clear away anything whose death
        // animation has finished playing.
        tick_combat(dt);
        tick_npc_telemetry(dt);

        // Is the player standing in a way out? Checked here, after physics and
        // locomotion have settled where they actually are this frame.
        tick_portals(snapshot);

        // Remember this frame's raw input for next frame's edge detection.
        prev_input_ = input;

        // The frame boundary: every mechanic tick and every entity loop is
        // finished, so swapping the world here cannot invalidate an iterator
        // somebody upstream is still holding. This is the ONLY place a
        // transition is performed, which is why ISceneTransition can only
        // request one.
        apply_pending_transition();

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

            // The eyes ride the rig as it is NOW -- after the player motor,
            // teleport and snap turn moved it this frame. The hands, the
            // avatar body and every Simulate-phase mechanic already used this
            // rig; the start-of-frame rig would draw the whole body one
            // frame's travel ahead of the camera.
            const Mat4 rig_render = rig_world_matrix();

            // One conservative frustum covering BOTH eyes, built from the HMD
            // centre pose with a deliberately padded FOV (Quest 3 is ~110 deg
            // horizontal; 140 deg here comfortably contains both eye frusta
            // plus the IPD offset). Culling once is cheaper than twice and,
            // being conservative, can only ever draw a few extra objects at
            // the very edge -- never wrongly remove one the player can see.
            Renderer::ViewInfo view_info{};
            if (real_head_valid) {
                constexpr float kCullFovRadians = 2.443f;  // 140 degrees
                const Mat4 world_from_head =
                    rig_render * xr_pose_to_matrix(real_head_pose);
                const Mat4 cull_proj = glm::perspective(
                    kCullFovRadians, 1.0f, 0.05f, 2000.0f);
                view_info.camera_pos = mat_translation(world_from_head);
                view_info.view_proj = cull_proj * glm::inverse(world_from_head);
                view_info.valid = true;
            }
            // Exact culling: the two eye frusta at this frame's predicted
            // display time -- the same poses the frame is rendered with.
            {
                std::array<XrView, 2> pv{};
                if (xr_.pending_views(pv)) {
                    const float near_z = 0.05f * rig_.scale;
                    const float far_z = 1000.0f * rig_.scale;
                    Vec3 mid(0.0f);
                    for (int e = 0; e < 2; ++e) {
                        const Mat4 world_from_eye =
                            rig_render * xr_pose_to_matrix(pv[static_cast<size_t>(e)].pose);
                        view_info.eye_view_proj[static_cast<size_t>(e)] =
                            xr_fov_to_projection(pv[static_cast<size_t>(e)].fov, near_z, far_z) *
                            glm::inverse(world_from_eye);
                        mid += mat_translation(world_from_eye) * 0.5f;
                    }
                    view_info.eyes_valid = true;
                    view_info.valid = true;
                    view_info.camera_pos = mid;
                    view_info.fov_y = pv[0].fov.angleUp - pv[0].fov.angleDown;
                    view_info.viewport_height_px =
                        static_cast<float>(xr_.swapchain_height() ? xr_.swapchain_height()
                                                                  : xr_.eye_height()) *
                        xr_.render_scale();
                }
            }
            {
                // The player's hands (and an avatar's feet) and what belongs
                // there, so the renderer can count anything else drawn on
                // top of them -- collider proxies once drew as white boxes.
                std::vector<Vec3> points;
                std::vector<std::uint32_t> allowed;
                const auto own = [&](entt::entity e) {
                    if (e != entt::null) allowed.push_back(static_cast<std::uint32_t>(e));
                };
                own(player_entity_);
                for (int h = 0; h < 2; ++h) {
                    own(hand_entities_[static_cast<size_t>(h)]);
                    own(finger_targets_[static_cast<size_t>(h)]);
                    own(hands_[static_cast<size_t>(h)].held);
                    if (hands_svc_ != nullptr) {
                        const std::uint32_t held = hands_svc_->held_entity(h);
                        const std::uint32_t support = hands_svc_->support_entity(h);
                        if (held != InputSnapshot::kInvalidEntity) allowed.push_back(held);
                        if (support != InputSnapshot::kInvalidEntity) allowed.push_back(support);
                    }
                    if (hand_valid[h]) points.push_back(hand_world_pos[h]);
                }
                const entt::entity avatar = finger_targets_[1] != entt::null ? finger_targets_[1]
                                                                             : finger_targets_[0];
                if (scene_.hands_source == "avatar" && avatar != entt::null && skeleton_svc_) {
                    const auto a = static_cast<std::uint32_t>(avatar);
                    for (const char* slot : {"foot_l", "foot_r"}) {
                        const int j = skeleton_svc_->joint_for_slot(a, slot);
                        if (j >= 0) points.emplace_back(skeleton_svc_->joint_world(a, j)[3]);
                    }
                }
                renderer_.set_player_focus(std::move(points), std::move(allowed));
            }
            renderer_.build_items_from_world(world_, items, view_info);
            {
                const auto& fs = renderer_.frame_stats();
                body_draws_max_ = std::max(body_draws_max_, fs.player_draws);
                placeholders_max_ = std::max(placeholders_max_, fs.placeholders);
                meshless_hidden_last_ = fs.meshless_hidden;
                if (fs.player_draws > 0 && body_draw_names_.empty()) {
                    for (const auto& n : renderer_.player_draw_names()) {
                        body_draw_names_ += (body_draw_names_.empty() ? "" : ",") + n;
                    }
                }
            }

            // Hand visuals. When the rigged hand meshes loaded, the hands are
            // ordinary entities that build_items_from_world has already drawn
            // through the skinned pipeline, so the only work left here is the
            // procedural fallback for a build with no generated hand asset.
            for (int h = 0; h < 2; ++h) {
                if (!hand_valid[h]) continue;
                if (hand_entities_[h] != entt::null) continue;
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

            // The teleport aim arc + landing marker used to be drawn here.
            // The teleport mechanic now submits them through IDebugDraw and
            // they arrive with the mechanic draw items below.

            // -------- Geometry submitted by mechanics via IDebugDraw -------
            // One-frame lifetime: mechanics re-submit every frame they want
            // something visible. Cleared at the top of the next frame.
            items.insert(items.end(), mech_draw_items_.begin(),
                         mech_draw_items_.end());

            const Renderer::RenderEnv* frame_env = &render_env_;
            Renderer::RenderEnv gallery_env;
            if (texture_gallery_) {
                if (!texture_gallery_->started()) texture_gallery_->start(world_, renderer_);
                texture_gallery_->update(dt);
                texture_gallery_->build_items(items, renderer_, last_head_world_pos_,
                                              last_head_forward_xz_);
                if (!texture_gallery_->done()) {
                    // A neutral key light from behind the eyes: a night
                    // scene lit the asset from behind and the check measured
                    // the darkness, not the texture. Same pipeline otherwise.
                    gallery_env = render_env_;
                    Vec3 key = last_head_forward_xz_ + Vec3(0.0f, -0.5f, 0.0f);
                    if (glm::length(key) < 1e-4f) key = Vec3(0.0f, -0.5f, -1.0f);
                    gallery_env.sun_direction = glm::normalize(key);
                    gallery_env.sun_color = Vec3(1.0f);
                    gallery_env.sun_intensity = std::max(render_env_.sun_intensity, 1.0f);
                    gallery_env.ambient_ground = Vec3(0.5f);
                    gallery_env.ambient_intensity = std::max(render_env_.ambient_intensity, 0.4f);
                    gallery_env.exposure = 1.0f;
                    gallery_env.fog_density = 0.0f;
                    gallery_env.points.clear();
                    frame_env = &gallery_env;
                }
            }

            xr_.render_frame(
                [&](const std::array<XrRuntime::EyeImage, 2>& eyes,
                    XrTime /*t*/) {
                    std::array<Renderer::EyeTarget, 2> targets{};
                    for (int e = 0; e < 2; ++e) {
                        targets[e].image = eyes[e].image;
                        targets[e].array_layer = eyes[e].array_layer;
                        targets[e].offset_x = eyes[e].offset_x;
                        targets[e].offset_y = eyes[e].offset_y;
                        targets[e].width = eyes[e].width;
                        targets[e].height = eyes[e].height;
                        targets[e].density_map = eyes[e].density_map;
                        targets[e].density_width = eyes[e].density_width;
                        targets[e].density_height = eyes[e].density_height;
                        Mat4 world_from_head =
                            rig_render * xr_pose_to_matrix(eyes[e].pose);
                        targets[e].view = glm::inverse(world_from_head);
                        // Clip planes scale with the player. A 2 cm-tall
                        // passenger with a fixed 5 cm near plane cannot see
                        // their own hands or the surface they are standing on
                        // -- both are inside it. The far plane scales too, so
                        // depth precision stays distributed over the range
                        // they can actually see rather than being spent on a
                        // kilometre they will never reach.
                        const float near_z = 0.05f * rig_.scale;
                        const float far_z = 1000.0f * rig_.scale;
                        targets[e].projection = xr_fov_to_projection(
                            eyes[e].fov, near_z, far_z);
                    }
                    // NOTE: Head pose cache (last_head_forward_xz_,
                    // last_head_world_pos_, last_head_pose_valid_) is
                    // populated up-front from the freshly-located VIEW
                    // space in sync_input/locomotion -- not here -- so
                    // locomotion logic always sees this frame's head
                    // pose, never the previous frame's render snapshot.
                    if (effects_) {
                        // Muzzle flashes and explosions light the scene for
                        // a few frames.
                        Renderer::RenderEnv env = *frame_env;
                        effects_->append_lights(env.points);
                        renderer_.draw_frame(targets, items, env);
                    } else {
                        renderer_.draw_frame(targets, items, *frame_env);
                    }
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

            const float frame_gpu_ms = renderer_.last_gpu_ms();
            const float frame_cpu_ms =
                std::max(0.0f, work_s * 1000.0f - renderer_.last_fence_wait_ms());
            if (governor_.update(work_s, frame_gpu_ms, frame_cpu_ms)) {
                const auto& q = governor_.state();
                renderer_.set_lod_bias(q.lod_bias);
                renderer_.set_cull_distance(q.cull_distance_m);
                xr_.set_foveation_level(q.foveation_level);
                xr_.set_render_scale(q.render_scale);
            }
            {
                // Memory: the engine's exact accounting plus queued loads,
                // against the sampled PSS. Frame time never feeds this.
                constexpr double kMb = 1.0 / (1024.0 * 1024.0);
                const auto mem = renderer_.memory_stats();
                MemoryGuard::Accounting acc{};
                acc.texture_mb = static_cast<double>(mem.texture_bytes) * kMb;
                acc.mesh_mb = static_cast<double>(mem.mesh_bytes) * kMb;
                acc.targets_mb = static_cast<double>(mem.target_bytes) * kMb;
                acc.frame_mb = static_cast<double>(mem.frame_bytes) * kMb;
                acc.reserved_mb = static_cast<double>(renderer_.pending_stream_bytes()) * kMb;
                if (memory_guard_.update(acc)) {
                    const int lvl = static_cast<int>(memory_guard_.level());
                    renderer_.set_memory_pressure(lvl);
                    if (lvl >= static_cast<int>(MemoryGuard::Level::Red)) {
                        MemoryGuard::purge_heap();
                    }
                }
            }

            telemetry_accum_s_ += dt;
            telemetry_work_accum_s_ += work_s;
            ++telemetry_frames_;
            telemetry_worst_frame_ms_ =
                std::max(telemetry_worst_frame_ms_, work_s * 1000.0f);
            // CPU = the frame's wall time minus the time spent blocked on the
            // GPU fence; GPU = timestamp delta of the last completed frame.
            const float gpu_ms = renderer_.last_gpu_ms();
            telemetry_cpu_accum_ms_ += std::max(
                0.0f, work_s * 1000.0f - renderer_.last_fence_wait_ms());
            telemetry_gpu_accum_ms_ += gpu_ms;
            telemetry_gpu_worst_ms_ = std::max(telemetry_gpu_worst_ms_, gpu_ms);
            if (telemetry_accum_s_ >= 1.0f && telemetry_frames_ > 0) {
                const auto& st = renderer_.frame_stats();
                const float frames = static_cast<float>(telemetry_frames_);
                // Animated agents are counted alongside the render stats
                // because they are the other half of the frame budget: a
                // scene can be well inside its triangle and draw-call
                // numbers and still miss frame rate on skeletons alone, and
                // without this line that looks like a rendering problem.
                int skeletons = 0;
                int posed = 0;
                {
                    auto sk_view = world_.registry().view<CSkeleton>();
                    for (auto e : sk_view) {
                        ++skeletons;
                        const auto* a =
                            world_.registry().try_get<CAnimator>(e);
                        if (a != nullptr && a->active_clip >= 0 && a->playing) {
                            ++posed;
                        }
                    }
                }
                PTV_INFO(
                    "perf: {:.1f} fps | work {:.1f} ms avg, {:.1f} ms worst | "
                    "cpu {:.1f} ms | gpu {:.1f} ms avg, {:.1f} ms worst | "
                    "calls {} pipes {} sets {} | "
                    "draw {} of {} (cull frustum {} dist {} small {} room {}, batches {}) | "
                    "tris {} | "
                    "meshes {} resident ({} tris), {} streaming | "
                    "rigs {} ({} animating, budget {}) | "
                    "lod_bias {:.1f} cull {:.0f}m ffr {} scale {:.2f}",
                    frames / telemetry_accum_s_,
                    (telemetry_work_accum_s_ * 1000.0f) / frames,
                    telemetry_worst_frame_ms_,
                    telemetry_cpu_accum_ms_ / frames,
                    telemetry_gpu_accum_ms_ / frames, telemetry_gpu_worst_ms_,
                    st.draw_calls, st.pipeline_binds, st.descriptor_binds,
                    st.drawn,
                    st.entities_considered, st.culled_frustum,
                    st.culled_distance, st.culled_small, st.culled_room, st.batches_drawn,
                    st.visible_triangles,
                    st.resident_meshes, st.resident_triangles,
                    st.streaming_pending, skeletons, posed,
                    scene_.performance.max_animated_agents,
                    renderer_.lod_bias(),
                    renderer_.cull_distance(), xr_.foveation_level(), xr_.render_scale());
                // What draws besides real meshes: placeholder boxes, meshless
                // entities left undrawn, and anything on the player's hands
                // or feet that is not theirs (max over the second).
                PTV_INFO("render_body: placeholders={} meshless_hidden={} player_draws={} "
                         "names={}",
                         placeholders_max_, meshless_hidden_last_, body_draws_max_,
                         body_draw_names_.empty() ? "-" : body_draw_names_);
                placeholders_max_ = 0;
                body_draws_max_ = 0;
                body_draw_names_.clear();
                // Weapons: live projectiles, effect quads drawn and casts.
                PTV_INFO("perf_fx: projectiles {} (cap {}) | fx_quads {} (cap {}) | "
                         "casts {}",
                         projectiles_ ? projectiles_->live_count() : 0,
                         scene_.performance.max_live_projectiles,
                         renderer_.fx_quads_drawn(), scene_.performance.max_fx_quads,
                         projectiles_ ? projectiles_->casts_last_tick() : 0);
                {
                    const auto& ts = renderer_.textures().stats();
                    const auto mem = renderer_.memory_stats();
                    constexpr double kMb = 1.0 / (1024.0 * 1024.0);
                    PTV_INFO("tex: resident {} | {:.1f} MB | ktx2 {} | decoded {} | "
                             "missing {} | missing_draws {} | astc {} | mesh {:.1f} MB | "
                             "targets {:.1f} MB | instances {}",
                             ts.resident, static_cast<double>(ts.bytes) * kMb, ts.ktx2,
                             ts.decoded, ts.missing, st.missing_texture_draws,
                             renderer_.textures().astc_supported() ? "yes" : "no",
                             static_cast<double>(mem.mesh_bytes) * kMb,
                             static_cast<double>(mem.target_bytes) * kMb, st.instances);
                }
                memory_guard_.add_evictions(renderer_.evictions() - logged_evictions_);
                logged_evictions_ = renderer_.evictions();
                PTV_INFO("{}", memory_guard_.telemetry_line());
#if defined(__ANDROID__)
                if (verify_mode_) {
                    // The verify pressure launch changes this mid-game.
                    char pv[PROP_VALUE_MAX] = {0};
                    float scale = 1.0f;
                    if (__system_property_get("debug.ptv.simulate_memory_pressure", pv) > 0) {
                        const float s = std::strtof(pv, nullptr);
                        if (s > 0.0f && s <= 1.0f) scale = s;
                    }
                    if (std::abs(scale - applied_pressure_scale_) > 1e-4f) {
                        applied_pressure_scale_ = scale;
                        memory_guard_.set_simulated_pressure(scale);
                    }
                }
#endif
                log_hands_telemetry(hand_valid);
                telemetry_accum_s_ = 0.0f;
                telemetry_work_accum_s_ = 0.0f;
                telemetry_frames_ = 0;
                telemetry_worst_frame_ms_ = 0.0f;
                telemetry_cpu_accum_ms_ = 0.0f;
                telemetry_gpu_accum_ms_ = 0.0f;
                telemetry_gpu_worst_ms_ = 0.0f;
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

    // Engineering A/B switch for on-device measurement only (never set by the
    // pipeline): debug.ptv.perf_override = "mv=0,fdm=0,fif=1,ffr=3,gov=0".
    {
        std::string ov;
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.ptv.perf_override", v) > 0) ov = v;
#else
        if (const char* e = std::getenv("PTV_PERF_OVERRIDE")) ov = e;
#endif
        if (!ov.empty() && ov != "0") {
            auto& p = scene_.performance;
            std::stringstream ss(ov);
            std::string kv;
            while (std::getline(ss, kv, ',')) {
                const auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = kv.substr(0, eq);
                const float val = std::strtof(kv.c_str() + eq + 1, nullptr);
                if (k == "mv") p.multiview = val != 0.0f;
                else if (k == "fdm") p.ffr_fdm = val != 0.0f;
                else if (k == "fif") p.frames_in_flight = static_cast<int>(val);
                else if (k == "ffr") {
                    p.foveation_level = static_cast<int>(val);
                    p.governor_max_foveation_level = static_cast<int>(val);
                } else if (k == "gov") p.governor_enabled = val != 0.0f;
                else if (k == "batch") p.static_batching = val != 0.0f;
                else if (k == "scale") p.render_scale_ceiling = val;
            }
            PTV_WARN("perf_override '{}': multiview={} fdm={} frames_in_flight={} ffr={} "
                     "governor={} batching={}", ov, p.multiview, p.ffr_fdm, p.frames_in_flight,
                     p.foveation_level, p.governor_enabled, p.static_batching);
        }
    }

    if (xr_ready) {
        PTV_INFO("App: OpenXR phase 2 (session + swapchains)...");
        XrRuntime::SwapchainOptions so{};
        so.multiview = scene_.performance.multiview;
        so.fragment_density_map = scene_.performance.ffr_fdm;
        so.render_scale_ceiling = scene_.performance.render_scale_ceiling;
        so.refresh_hz = static_cast<float>(scene_.performance.refresh_hz);
        xr_.set_swapchain_options(so);
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
                rig_.yaw_rad = yaw_for_facing(f);
            }
        }
        ApplySceneToWorld(scene_, world_);
        resolve_project_mesh_paths(world_, scene_path_);
        physics_.rebuild_from_world(world_);
        build_nav_graph();

        apply_player_settings_from_scene();
    } else {
        PTV_WARN("No scene.json provided. The engine will run idle.");
    }

    // The player needs a body before any mechanic can attach, because
    // locomotion sweeps against it and gameplay code looks the user up by
    // the "player" tag. Created after the scene so it picks up the authored
    // height and spawn point.
    read_verify_mode();
    create_player_body();

    // Cache the renderer environment (sky/sun/ambient/fog/point lights) from
    // the loaded scene so the renderer honors what the world-gen agent
    // authored. Safe to call whether or not a scene was loaded.
    build_render_env();

    // Stand up the services mechanics are allowed to call, then attach.
    // Must precede attach_bindings: on_attach receives the context.
    locomotion_svc_ = std::make_unique<LocomotionService>(this);
    physics_svc_ = std::make_unique<PhysicsAccessService>(this);
    contacts_svc_ = std::make_unique<ContactsService>(this);
    spawn_svc_ = std::make_unique<SpawnService>(this);
    attach_svc_ = std::make_unique<AttachService>(this);
    combat_svc_ = std::make_unique<CombatService>(this);
    perception_svc_ = std::make_unique<PerceptionService>(this);
    nav_svc_ = std::make_unique<NavigationService>(this);
    draw_svc_ = std::make_unique<DebugDrawService>(this);
    hands_svc_ = std::make_unique<HandsService>(this);
    fx_svc_ = std::make_unique<TransformFxService>(this);
    haptics_svc_ = std::make_unique<HapticsService>(this);
    sockets_svc_ = std::make_unique<SocketsService>(this);
    skeleton_svc_ = std::make_unique<SkeletonService>(this);
    // After attach_svc_, which it drives.
    equip_svc_ = std::make_unique<EquipService>(this);
    interactive_svc_ = std::make_unique<InteractiveService>(this);
    persistence_svc_ = std::make_unique<PersistenceService>(this);
    transition_svc_ = std::make_unique<TransitionService>(this);
    ui_svc_ = std::make_unique<UiService>(this);
    comfort_svc_ = std::make_unique<ComfortService>(this);
    app_svc_ = std::make_unique<AppService>(this);
    text_svc_ = std::make_unique<TextDrawService>(this);
    effects_ = std::make_unique<EffectsSystem>();
    projectiles_ = std::make_unique<ProjectileSystem>();
    weapons_hands_ = hands_svc_.get();
    weapons_ = std::make_unique<WeaponBoard>(&weapons_hands_);
    mech_ctx_.vfx = effects_.get();
    mech_ctx_.projectiles = projectiles_.get();
    mech_ctx_.weapons = weapons_.get();
    configure_weapon_systems();
    mech_ctx_.locomotion = locomotion_svc_.get();
    mech_ctx_.comfort = comfort_svc_.get();
    mech_ctx_.game = &game_flow_board_;
    mech_ctx_.app = app_svc_.get();
    mech_ctx_.text = text_svc_.get();
    mech_ctx_.physics = physics_svc_.get();
    mech_ctx_.contacts = contacts_svc_.get();
    mech_ctx_.spawn = spawn_svc_.get();
    mech_ctx_.encounters = &encounter_board_;
    mech_ctx_.attach = attach_svc_.get();
    mech_ctx_.combat = combat_svc_.get();
    mech_ctx_.perception = perception_svc_.get();
    mech_ctx_.nav = nav_svc_.get();
    mech_ctx_.draw = draw_svc_.get();
    mech_ctx_.hands = hands_svc_.get();
    mech_ctx_.fx = fx_svc_.get();
    mech_ctx_.haptics = haptics_svc_.get();
    mech_ctx_.sockets = sockets_svc_.get();
    mech_ctx_.skeleton = skeleton_svc_.get();
    mech_ctx_.equip = equip_svc_.get();
    mech_ctx_.interactive = interactive_svc_.get();
    mech_ctx_.persistence = persistence_svc_.get();
    mech_ctx_.transition = transition_svc_.get();
    mech_ctx_.ui = ui_svc_.get();
    signal_board_ = std::make_unique<SignalBoard>();
    var_board_ = std::make_unique<VarBoard>();
    anchor_board_ = std::make_unique<AnchorBoard>();
    // Bound to the scene document's own vectors, which every scene load
    // refills in place.
    anchor_board_->bind(&scene_.anchors, &scene_.anchor_sets, &world_);
    mech_ctx_.signals = signal_board_.get();
    mech_ctx_.vars = var_board_.get();
    mech_ctx_.anchors = anchor_board_.get();

    check_player_mechanic_roles();
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
        renderer_.set_frames_in_flight(scene_.performance.frames_in_flight);
        Renderer::TargetInfo target{};
        target.color_format = xr_.color_format();
        target.width = xr_.swapchain_width() ? xr_.swapchain_width() : xr_.eye_width();
        target.height = xr_.swapchain_height() ? xr_.swapchain_height() : xr_.eye_height();
        target.multiview = xr_.multiview();
        target.fragment_density_map = xr_.fragment_density_map();
        renderer_ready_ = renderer_.initialize(vk_, target);
        PTV_INFO("render: {}x{} per eye, multiview={} ffr_fdm={} frames_in_flight={} "
                 "refresh={:.0f}Hz",
                 target.width, target.height, target.multiview ? "on" : "off",
                 target.fragment_density_map ? "on" : "off", renderer_.frames_in_flight(),
                 xr_.refresh_rate());
        // A texture that should exist but failed to load draws as a magenta
        // checker in verification runs and debug builds, so it cannot pass
        // for an intentionally untextured surface; release builds fall back
        // to the material's own colour (the failure is logged either way).
#if defined(NDEBUG)
        renderer_.set_texture_diagnostics(verify_mode_);
#else
        renderer_.set_texture_diagnostics(true);
#endif
        // Baseline foveation for this scene's tier. No-op on runtimes without
        // the XR_FB_foveation extensions (e.g. PC VR over Quest Link).
        xr_.set_foveation_level(scene_.performance.foveation_level);
        if (!renderer_ready_) {
            PTV_WARN("Renderer init failed; headset will not display content.");
        }
        if (renderer_ready_) {
            attach_skeletons();
            renderer_.build_static_batches(world_, scene_.nav_regions, scene_.nav_links);
        }
    }
    memory_guard_.configure(scene_.performance);
    {
        // Verification only: lower every threshold mid-game so the guard's
        // red level and recovery can be exercised on a small game.
        float scale = 1.0f;
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.ptv.simulate_memory_pressure", v) > 0) {
            scale = std::strtof(v, nullptr);
        }
#else
        if (const char* e = std::getenv("PTV_SIMULATE_MEMORY_PRESSURE")) scale = std::strtof(e, nullptr);
#endif
        if (scale > 0.0f && scale < 1.0f) {
            memory_guard_.set_simulated_pressure(scale);
            applied_pressure_scale_ = scale;
        }
    }
    memory_guard_.start();
    MemoryGuard::purge_heap();
    // The hands belong to the player, not the scene, so they are created even
    // when no scene loaded -- that is exactly the case where you want to see
    // whether tracking works.
    if (renderer_ready_) create_hand_entities();
    return true;
}

void Application::apply_player_settings_from_scene() {
    slide_max_speed_mps_ = scene_.slide_max_speed_mps;
    slide_accel_tau_s_ = scene_.slide_accel_tau_s;
    slide_decel_tau_s_ = scene_.slide_decel_tau_s;
    sprint_speed_mps_ = scene_.sprint_speed_mps;
    sprint_accel_tau_s_ = scene_.sprint_accel_tau_s;
    sprint_decel_tau_s_ = scene_.sprint_decel_tau_s;
    sprint_forward_cone_deg_ = scene_.sprint_forward_cone_deg;
    sprint_min_stick_ = scene_.sprint_min_stick;
    sprint_activation_ =
        scene_.sprint_activation == "off"
            ? ::ptv::SprintActivation::Off
            : scene_.sprint_activation == "toggle"
                  ? ::ptv::SprintActivation::Toggle
                  : ::ptv::SprintActivation::Hold;
    sprinting_ = false;
    gait_speed_cap_ = slide_max_speed_mps_;
    teleport_launch_speed_mps_ = scene_.teleport_launch_speed_mps;
    teleport_max_horizontal_m_ = scene_.teleport_max_horizontal_m;
    teleport_floor_y_ = scene_.teleport_floor_y;
    snap_turn_degrees_ = scene_.snap_turn_degrees;
    snap_turn_cooldown_s_ = scene_.snap_turn_cooldown_s;

    auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string m = lower(scene_.default_locomotion_mode);
    if (m.find("controller") != std::string::npos) {
        locomotion_ = LocomotionMode::SlideControllerRelative;
    } else if (m.find("head") != std::string::npos || m.find("slide") !=
                                                          std::string::npos) {
        locomotion_ = LocomotionMode::SlideHeadRelative;
        } else {
        locomotion_ = LocomotionMode::Teleport;
    }
    turn_mode_ = (lower(scene_.default_turn_mode) == "smooth")
                     ? ::ptv::TurnMode::Smooth
                     : ::ptv::TurnMode::Snap;

    // Nothing is available until a mechanic says so. A stationary game binds
    // no locomotion at all, and that is a complete, valid game.
    mode_available_ = {false, false, false};
    turn_available_ = {false, false};
    input_lock_owners_.clear();
    vignette_channels_ = {};
    vertical_mode_ = ::ptv::VerticalMode::Grounded;
    pending_move_ = Vec3(0.0f);
    pending_jump_mps_ = 0.0f;
    motor_needs_sync_ = true;
}

void Application::reconcile_available_modes() {
    // A game whose default mode is not one it actually binds (a teleport-only
    // game authored with a slide default, say) would otherwise leave the stick
    // doing nothing. Fall back to the first mode that exists.
    const auto idx = static_cast<size_t>(locomotion_);
    const bool any_mode =
        mode_available_[0] || mode_available_[1] || mode_available_[2];
    if (any_mode && !mode_available_[idx]) {
        for (size_t i = 0; i < mode_available_.size(); ++i) {
            if (mode_available_[i]) {
                locomotion_ = static_cast<LocomotionMode>(i);
                break;
            }
        }
    }
    const auto tidx = static_cast<size_t>(turn_mode_);
    const bool any_turn = turn_available_[0] || turn_available_[1];
    if (any_turn && !turn_available_[tidx]) {
        turn_mode_ = turn_available_[0] ? ::ptv::TurnMode::Snap
                                        : ::ptv::TurnMode::Smooth;
    }
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
