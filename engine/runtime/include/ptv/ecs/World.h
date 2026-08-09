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
    /** One flag per joint, set by ``ISkeleton`` writes and cleared at the end
     *  of each tick. A set flag means "the clip must not touch this joint
     *  this frame". */
    std::vector<std::uint8_t> joint_overridden;
};

struct CPhysics {
    std::string kind{"static"};
    float mass_kg{1.0f};
    float restitution{0.2f};
    float friction{0.5f};
    std::string collision_shape{"aabb"};
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
