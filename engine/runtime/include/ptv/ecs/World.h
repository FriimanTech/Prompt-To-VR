#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ptv/math/Math.h"
#include "ptv/render/GltfSkinnedLoad.h"

namespace ptv {

class IMechanic;

/** Lightweight wrapper around an EnTT registry + stable ids. */
class World {
public:
    World() = default;

    entt::registry& registry() { return registry_; }
    const entt::registry& registry() const { return registry_; }

    entt::entity create_entity(const std::string& id = {}) {
        auto e = registry_.create();
        if (!id.empty()) id_to_entity_[id] = e;
        return e;
    }

    entt::entity find(const std::string& id) const {
        auto it = id_to_entity_.find(id);
        return it == id_to_entity_.end() ? entt::null : it->second;
    }

    float delta_time() const { return dt_; }
    void set_delta_time(float dt) { dt_ = dt; }

    // ---- uint32_t-safe component helpers for generated code ---------------
    //
    // IMechanic's interface passes entities as std::uint32_t (a stable ABI
    // for generated code). entt::registry wants ``entt::entity``. Wrapping
    // the cast once here means mechanics never have to write
    // ``static_cast<entt::entity>(e)`` and can't accidentally truncate.

    template <class C>
    bool has(std::uint32_t e) const {
        return registry_.all_of<C>(static_cast<entt::entity>(e));
    }

    template <class... Cs>
    bool has_all(std::uint32_t e) const {
        return registry_.all_of<Cs...>(static_cast<entt::entity>(e));
    }

    template <class C>
    C& get(std::uint32_t e) {
        return registry_.get<C>(static_cast<entt::entity>(e));
    }

    template <class C>
    const C& get(std::uint32_t e) const {
        return registry_.get<C>(static_cast<entt::entity>(e));
    }

    template <class C, class... Args>
    C& emplace_or_replace(std::uint32_t e, Args&&... args) {
        return registry_.emplace_or_replace<C>(
            static_cast<entt::entity>(e), std::forward<Args>(args)...);
    }

    template <class C>
    void remove(std::uint32_t e) {
        registry_.remove<C>(static_cast<entt::entity>(e));
    }

    bool valid(std::uint32_t e) const {
        return registry_.valid(static_cast<entt::entity>(e));
    }

private:
    entt::registry registry_{};
    std::unordered_map<std::string, entt::entity> id_to_entity_{};
    float dt_{0.0f};
};

// ---- Core components --------------------------------------------------

struct CTransform {
    Transform t;
};

/** Purely visual, springy offset applied on top of ``CTransform`` at draw
 * time. Never seen by physics, grab, raycasts or collision.
 *
 * This exists because ``CTransform`` is an OUTPUT of the physics step for any
 * entity with a body: ``PhysicsWorld::sync_to_world`` overwrites position and
 * rotation from the Jolt body every frame. A mechanic that wants a weapon to
 * kick when it fires therefore cannot write ``CTransform`` -- it would either
 * be erased by the next sync or silently desync the drawn pose from the
 * physical one. The offset lives downstream of everyone who cares about where
 * the entity "really" is, so nothing fights over ownership.
 *
 * Drive it through ``MechanicContext::fx`` rather than writing it directly;
 * the service clamps the magnitudes. */
struct CTransformOffset {
    /** Current offset, in the entity's local space. */
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    /** Rest pose the offset springs back to. Almost always identity; a
     * non-zero target holds a permanent lean/tilt. */
    Vec3 target_position{0.0f};
    Quat target_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    /** Exponential recovery time constant in seconds. */
    float recover_tau_s{0.06f};
    /** False once the offset has settled onto its target; the tick skips
     * settled entities and the renderer skips the matrix multiply. */
    bool active{false};
};

struct CMesh {
    std::string asset_id;
    std::string glb_path;
    float target_scale_meters{1.0f};
    /** Decimated LOD chain (coarsest last) from the asset_optimization
     * pipeline stage; ``lod_paths[0]`` == ``glb_path``. Empty when the asset
     * was never optimized, in which case the renderer always draws
     * ``glb_path``. */
    std::vector<std::string> lod_paths;
    /** World-space bounding-sphere radius (metres) used for frustum and
     * distance culling. 0 = unknown -> renderer uses a safe fallback. */
    float bounds_radius_m{0.0f};
    /** Named attachment points ("muzzle", "grip") in the mesh's local space,
     * in metres. Read through ``MechanicContext::sockets`` rather than
     * directly, so a missing socket degrades instead of returning identity. */
    std::unordered_map<std::string, Transform> sockets;
    /** True when the asset is rigged. The scene loader uses this to decide
     * whether to attach a ``CSkeleton``; entities without it never touch the
     * skinned pipeline. */
    bool skinned{false};
    /** Animation intent ("walk") -> the real glTF clip name in this asset.
     * Read through ``MechanicContext::skeleton`` rather than directly. */
    std::unordered_map<std::string, std::string> animations;
    /** Animation intent -> the ground speed, in m/s, that the clip LOOKS like
     * it is travelling at.
     *
     * Tripo retargets with ``animate_in_place=True``, so a walk clip has no
     * root motion to measure -- the feet cycle but the root never moves. The
     * scene compiler therefore derives an apparent speed from the character's
     * height and writes it here. ``builtin.character_locomotion`` divides the
     * entity's real speed by it to set the playback rate, which is what stops
     * the feet from sliding over the floor. Empty means "unknown", and the
     * mechanic falls back to a height-derived estimate. */
    std::unordered_map<std::string, float> animation_speeds;
};

/** Skeleton for a rigged ``CMesh``, plus the palette the renderer uploads.
 *
 * Joint indices match the source glTF skin's ``joints`` array exactly,
 * because the mesh's ``JOINTS_0`` attribute references that ordering. Walk
 * ``skeleton.order`` when you need parents before children.
 *
 * Attached by the scene loader only for GLBs that actually carry a skin;
 * entities without one are drawn by the plain static pipeline and cost
 * nothing extra. */
struct CSkeleton {
    /** Hierarchy, bind pose and inverse bind matrices from the GLB. */
    gltf_mesh_load::SkeletonData skeleton;
    /** The pose being rendered, one local transform per joint. Reset to the
     *  bind pose at the top of every ``tick_animators`` pass, then written by
     *  the active clip and by any procedural override. */
    std::vector<gltf_mesh_load::JointPose> local_pose;
    /** ``jointWorld * inverseBind`` per joint, rebuilt each frame and copied
     *  straight into the renderer's joint buffer. */
    std::vector<Mat4> palette;
    /** Socket name -> the joint it rides on, resolved once at attach.
     *
     *  ``CMesh::sockets`` are positions in the mesh's bind pose, which is
     *  correct for a static prop and wrong the instant a character animates:
     *  a ``muzzle`` derived from a goblin holding a rifle would stay frozen
     *  in mid-air while the goblin's arm swung through a walk cycle. Binding
     *  each socket to its nearest joint lets ``socket_world`` push it through
     *  the same palette entry the surrounding vertices use, so it tracks the
     *  limb exactly as the geometry does. */
    std::unordered_map<std::string, int> socket_joint;
};

/** Clip playback plus per-joint procedural override for a ``CSkeleton``.
 *
 * The two coexist by design. A clip is motion recorded against time; an
 * override is a pose computed this frame from something live, typically
 * controller input driving finger curl. Both write into the same
 * ``CSkeleton::local_pose``, and an overridden joint simply wins over the
 * clip for that frame, which is what lets a character play an idle animation
 * while code aims its head or its trigger finger. */
struct CAnimator {
    std::vector<gltf_mesh_load::AnimationClip> clips;
    /** Index into ``clips``, or -1 for no clip (pure procedural posing). */
    int active_clip{-1};
    float time_s{0.0f};
    bool looping{true};
    bool playing{true};
    float speed{1.0f};
    /** The clip being blended OUT of, or -1 when not cross-fading.
     *
     * Switching clips used to be a hard cut, which reads as a visible snap
     * every time a character starts or stops walking. The outgoing clip keeps
     * advancing on its own time so the blend is between two moving poses
     * rather than between a moving pose and a frozen one. */
    int prev_clip{-1};
    float prev_time_s{0.0f};
    /** Seconds remaining in the current cross-fade. */
    float blend_t{0.0f};
    /** Total duration of the current cross-fade, for normalising ``blend_t``. */
    float blend_duration{0.0f};
    /** Elapsed time owed to this animator because distance LOD skipped it.
     *
     * Paid back in full on the next frame it IS sampled, so a character far
     * from the player still walks at the right speed -- only the sampling is
     * coarser. Without this, throttling would visibly slow distant NPCs
     * down. */
    float lod_debt_s{0.0f};
    /** One flag per joint, set by ``ISkeleton`` writes and cleared at the end
     *  of each tick. A set flag means "the clip must not touch this joint
     *  this frame". */
    std::vector<std::uint8_t> joint_overridden;
};

/** What a character WANTS to do this frame; ``builtin.character_locomotion``
 * turns it into actual movement and the matching animation.
 *
 * This is the seam between AI and locomotion, and the reason generated code
 * never has to think about clips at all. A behaviour decides "go that way"
 * and writes a velocity here; the built-in moves the body, turns it to face
 * travel, picks idle / walk / run from the resulting speed, and scales the
 * clip's playback rate so the stride matches the ground covered. Generated
 * code that instead called ``play_clip`` itself would have to re-derive all
 * of that, and would get the foot-sliding wrong.
 *
 *     auto& want = world.emplace_or_replace<CLocomotionIntent>(self);
 *     want.desired_velocity = direction * 3.0f;
 */
struct CLocomotionIntent {
    /** World-space velocity the character should move at, in m/s. The
     *  built-in clamps it to the configured walk/run ceiling, so a behaviour
     *  can simply ask for "as fast as possible" with a large vector. */
    Vec3 desired_velocity{0.0f};
    /** Prefer the run clip even at a modest speed (a scared walk-run). */
    bool sprint{false};
    /** Turn to face the direction of travel. Clear it to keep facing a
     *  target while strafing or backing away -- then set ``face_target``. */
    bool face_travel{true};
    /** World point to face when ``face_travel`` is false. An enemy that
     *  circles the player while keeping its weapon trained uses this. */
    Vec3 face_target{0.0f};
    bool has_face_target{false};
};

struct CPhysics {
    std::string kind{"static"};
    float mass_kg{1.0f};
    float restitution{0.2f};
    float friction{0.5f};
    std::string collision_shape{"aabb"};
};

/** Hit points, plus the small amount of state dying needs.
 *
 * In the engine rather than in generated code because damage is the one
 * mechanic every combat game re-invents, and re-invents differently: without
 * a shared component an enemy written by one CodeTask cannot hurt a player
 * written by another. Drive it through ``MechanicContext::combat``. */
struct CHealth {
    float current{100.0f};
    float max{100.0f};
    /** Remaining invulnerability, in seconds. Exists because contact damage
     *  is evaluated every frame: without it, one touch from an enemy drains
     *  a health bar in a fraction of a second. */
    float invuln_s{0.0f};
    /** Cooldown applied after each hit lands. */
    float invuln_after_hit_s{0.4f};
    bool alive{true};
    /** Counts down while the death animation plays; the entity is removed
     *  when it reaches zero. Negative means "not dying". */
    float death_timer_s{-1.0f};
};

/** Which side an entity is on.
 *
 * Needed for the case the brief calls out: enemies must fight the player's
 * allied NPCs too, not only the player. Target selection asks the faction,
 * so an ally and an enemy written by different CodeTasks still recognise
 * each other as opponents. */
struct CFaction {
    std::string id{"neutral"};
    std::vector<std::string> hostile_to;
};

/** Are these two factions on opposing sides?
 *
 * Symmetric on purpose: it is enough for EITHER side to name the other as
 * hostile. An asymmetric answer produces the classic bug where enemies shoot
 * the player's allies but the allies never shoot back, because only one of
 * the two CodeTasks that authored them thought to fill in ``hostile_to``. */
inline bool factions_hostile(const CFaction& a, const CFaction& b) {
    if (a.id == b.id) return false;
    for (const auto& s : a.hostile_to) {
        if (s == b.id) return true;
    }
    for (const auto& s : b.hostile_to) {
        if (s == a.id) return true;
    }
    return false;
}

/** Rigidly parents one entity to a point on another.
 *
 * This is what puts a separately-generated weapon into a separately-generated
 * character's hand. Tripo sometimes produces "goblin holding a rifle" as one
 * mesh and sometimes produces a goblin and a rifle as two, and a VR engine
 * that can only handle the first case cannot build half the games it is
 * asked for.
 *
 * Resolved after the pose system runs, so the child follows the parent's
 * ANIMATED joint rather than its bind pose -- a rifle attached to a hand
 * swings with the walk cycle.
 *
 * Drive it through ``MechanicContext::attach`` rather than writing it
 * directly; the service validates the anchor and takes the child out of
 * physics simulation. */
struct CAttachment {
    /** Entity this one rides on. */
    std::uint32_t parent{0xFFFFFFFFu};
    /** Socket or joint on the parent to follow. */
    std::string anchor;
    /** Extra offset applied after the anchor, in the anchor's space. */
    Vec3 local_position{0.0f};
    Quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct CMechanicBinding {
    struct Entry {
        std::string id;              // fully-qualified mechanic id
        std::string builtin_id;      // populated for builtin / builtin+config
        std::string params_json;     // serialized params for deterministic re-init
    };
    std::vector<Entry> entries;
};

struct CTag {
    std::vector<std::string> tags;
};

// ---- Canonical tag helper --------------------------------------------------
//
// Generated mechanics frequently need "does this entity have tag X?".
// Providing it ONCE here (inline, in the public engine API) gives generated
// code a canonical helper so tasks stop defining their own ``entity_has_tag``
// free function with external linkage -- which collides at link time
// (LNK2005 "already defined") when two parallel-generated tasks pick the same
// name. Generated code should call ``has_tag`` instead of rolling its own.
// (``add``/``remove`` are intentionally NOT provided as free functions to
// avoid clashing with helpers some generated tasks already define; mutate
// ``world.get<CTag>(e).tags`` directly, or use a task-unique helper name.)

inline bool has_tag(const World& world, std::uint32_t e, std::string_view tag) {
    if (!world.has<CTag>(e)) return false;
    for (const auto& t : world.get<CTag>(e).tags) {
        if (t == tag) return true;
    }
    return false;
}

// Procedural primitive geometry authored by the world-generation stage.
// Rendered as a colored box/plane/cylinder/sphere/ramp sized by ``size``.
struct CPrimitive {
    std::string kind{"box"};  // box | plane | cylinder | sphere | ramp
    Vec3 size{1.0f, 1.0f, 1.0f};
    Vec3 color{0.8f, 0.8f, 0.82f};
};

// Arbitrary procedural triangle mesh authored by the world-generation stage.
// ``positions``/``normals`` are flat xyz triples; ``indices`` is a triangle
// list. ``normals`` may be empty (renderer/collision compute flat normals).
struct CInlineMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<std::uint32_t> indices;
    Vec3 color{0.8f, 0.8f, 0.82f};
};

}  // namespace ptv
