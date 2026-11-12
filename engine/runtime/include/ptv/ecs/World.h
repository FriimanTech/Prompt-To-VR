#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ptv/api/InteractiveState.h"
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

    /** The scene id of ``e``, or nullptr for an entity created without one.
     *
     *  Needed by anything that has to write a durable reference to an
     *  entity. The raw entt handle is NOT durable: handles are reassigned on
     *  every load, so a save keyed on them restores the wrong objects. */
    const std::string* id_of(entt::entity e) const {
        for (const auto& [id, entity] : id_to_entity_) {
            if (entity == e) return &id;
        }
        return nullptr;
    }

    /** Empty the world completely, for a scene swap.
     *
     *  The id map MUST go with the entities. entt reuses handles, so a name
     *  left over from the outgoing scene would resolve to whatever entity
     *  happens to take that handle in the new one -- and it would resolve
     *  successfully, which is the dangerous part: a lookup for "front_door"
     *  would silently return a lamp. */
    void clear() {
        registry_.clear();
        id_to_entity_.clear();
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

/** Per-socket facts from ``socket_generation`` (kind, mirror rule, sizes),
 *  beside ``CMesh::sockets``. Absent on meshes from older pipelines. */
struct SocketInfo {
    std::string kind;
    std::string mirror;
    std::string wrap_of;
    float lateral_offset_m{0.0f};
    float bore_diameter_m{0.0f};
    float handle_thickness_m{0.0f};
};

struct CSocketInfo {
    std::unordered_map<std::string, SocketInfo> sockets;
};

enum class HoldKind : int { None, OneHand, TwoHandLine, TwoHandWrap, ForearmStrap };
enum class TwoHandMode : int { None, Optional, Required };
enum class HoldFingerPose : int { Wrap, Trigger, Strap, Pinch, OpenSupport };

/** How a held item is solved into the hand(s), from ``mesh.hold``.
 *  ``builtin.grab`` reads it; ``builtin.hand_pose`` reads ``finger_pose``. */
struct CHoldProfile {
    HoldKind kind{HoldKind::None};
    bool aim_aligned{false};
    TwoHandMode two_hand{TwoHandMode::None};
    std::string primary_socket;
    std::string support_socket;
    std::string forearm_socket;
    HoldFingerPose finger_pose{HoldFingerPose::Wrap};
    /** Mesh-space facing of aim-aligned items (where shots go, and up). */
    bool has_forward{false};
    Vec3 forward{0.0f, 0.0f, 1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
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
    /** Metres per source unit, measured ONCE from the asset's LOD0 by the
     *  asset_optimization stage. When set, every LOD is loaded with exactly
     *  this factor instead of each file re-normalizing itself to
     *  ``target_scale_meters``, so all levels are guaranteed the same size.
     *  0 = unknown (older scenes): per-file normalization. Last member so
     *  positional initializers written before it existed stay correct. */
    float unit_scale{0.0f};
    /** Distance in metres from the origin DOWN to the soles in the rest pose,
     *  at the asset's in-scene size. A grounded character is planted this far
     *  above the floor. For a character with a model frame this is always
     *  half its height (the frame pivots the soles there). 0 = unknown: the
     *  character locomotion measures the loaded bind pose instead. */
    float foot_offset_m{0.0f};
    /** Measured model frame (``asset_contract``), GLB units. When set, the
     *  mesh is scaled by HEIGHT (``unit_scale``), rotated so its face points
     *  down -Z and dropped so its soles sit ``foot_offset_m`` below the
     *  origin. Rigged meshes get it folded into their root joints at load;
     *  static ones get it on the draw's model matrix. */
    bool has_model_frame{false};
    float model_yaw_offset_rad{0.0f};
    float model_sole_y{0.0f};
    float model_height{0.0f};
    /** Per-LOD simplification error relative to the mesh extent (see
     *  MeshRef::lod_errors_rel). Empty = switch by distance. */
    std::vector<float> lod_errors_rel;
};

/** The collider measured from a GLB prop's own mesh by the asset pipeline,
 *  in metres in entity space at transform scale 1 (the same space the
 *  renderer draws the mesh in). Physics builds the body from this:
 *  ``mesh_lod``'s triangles for a static ``"mesh"`` body, the convex ``hull``
 *  for anything that moves, the offset box for ``"aabb"``. */
struct CCollisionGeom {
    Vec3 aabb_min{0.0f};
    Vec3 aabb_max{0.0f};
    std::vector<Vec3> hull;
    /** Index into ``CMesh::lod_paths`` of the static triangle collider, or -1. */
    int mesh_lod{-1};
};

/** Rendering-only: this entity was marked static by the scene compiler.
 *  ``batch`` is the merged draw it belongs to (-1 = drawn on its own; set
 *  back to -1 if the entity ever moves), ``room`` the closed room it stands
 *  in (-1 = none), for room culling. */
struct CStaticBatch {
    int batch{-1};
    int room{-1};
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
    /** Second palette for the player's OWN view, or empty when unused.
     *
     *  Built only for entities carrying ``CSelfViewHide``. Two palettes exist
     *  because head visibility is a per-view decision: the camera sits at the
     *  avatar's eyes, so the head must not render in the player's own view,
     *  but it must still render in a mirror -- and deleting it once would
     *  make the reflection headless. This is the same approach the VRM
     *  specification calls ``thirdPersonOnly`` and VRChat calls Head Chop:
     *  render the avatar differently per view rather than removing geometry
     *  globally. */
    std::vector<Mat4> palette_self_view;
};

/** Joints to collapse in the player's own view only.
 *
 * Holds a whole subtree rather than a single joint because hiding a head
 * means hiding the jaw, eyes and hair with it -- suppress only the head joint
 * and the player sees their own hair from the inside.
 *
 * ``scale`` is what makes the toggle case work: a runtime control animating
 * it between 0 and 1 shows or hides the head without rebuilding anything,
 * mirroring VRChat's global scale factor. */
struct CSelfViewHide {
    std::vector<int> joints;
    /** 0 collapses the subtree to a point; 1 leaves it fully visible. */
    float scale{0.0f};
};

/** One selectable line on a virtual panel. */
struct UiRow {
    /** Drawn on the row by the panel's text pass ("Try Again"). */
    std::string label;
    /** Stable identifier the caller matches events against. Using this rather
     *  than the row INDEX is what lets a panel be rebuilt -- an inventory that
     *  reorders itself would otherwise deliver a click for the wrong item. */
    std::string id;
    /** An entity whose mesh is shown on the row, or kInvalidUiEntity.
     *
     *  For a wardrobe this is the actual item, which beats a text label in VR:
     *  the player recognises the gauntlet by looking at it. */
    std::uint32_t thumbnail{0xFFFFFFFFu};
    /** Greyed out and unclickable. A helmet that cannot be removed in flight
     *  still LISTS -- hiding it would leave the player hunting for something
     *  they can see on their own body. */
    bool enabled{true};
    /** The caller's current choice. */
    bool selected{false};
    /** A persistent property of the thing, not of the interaction: "equipped"
     *  in a wardrobe, "owned" in a shop. Distinct from ``selected``, which is
     *  what the pointer is on right now -- the two have to be separable or a
     *  wardrobe cannot show you inspecting one item while wearing another. */
    bool marked{false};
};

/** A click the panel recorded, waiting to be read. */
struct UiEvent {
    std::string panel_id;
    std::string row_id;
    int row_index{-1};
    /** Which hand clicked, for haptics or handedness-aware logic. */
    int hand{-1};
};

/**
 * A world-anchored virtual panel.
 *
 * Generalized out of the locomotion menu, which proved the pattern but was
 * hard-coded in the application layer: fixed pages, two pads, and its own
 * copies of edge detection and debounce. Any generated menu -- a wardrobe, a
 * mission select, a shop -- needed all of that again from scratch, over a
 * MechanicContext that offered only debug draw and raycasts.
 *
 * Rows carry no positions. The panel lays them out, because a caller that
 * placed its own rows would have to re-place them every time the list changed
 * length, and would have no way to keep two panels comfortable at once.
 */
struct CUiPanel {
    /** Addressed by generated code through ``IUi``. */
    std::string id;
    std::vector<UiRow> rows;

    bool open{false};

    /** Placed once when opened, then left alone. Re-anchoring on every change
     *  would jerk the panel around mid-decision, which is exactly when the
     *  player is reading it. */
    Vec3 anchor{0.0f, 1.55f, -1.2f};
    float yaw_rad{0.0f};
    bool anchor_set{false};

    /** Meta's "Head -> Comfort" guidance: primary UI about 1.5 m forward and
     *  slightly below eye level, so the natural line of sight rests on it
     *  without neck strain. At 1.5 m, 10 degrees below the horizon is ~0.20 m. */
    float distance_m{1.5f};
    float below_eye_m{0.20f};

    float row_height_m{0.12f};
    float width_m{0.42f};
    /** How close a pointer ray must pass to a row's centre to select it. */
    float hit_radius_m{0.09f};

    /** -1 = either hand may point. Pinning it matters for a panel meant to be
     *  held in one hand and operated with the other. */
    int pointer_hand{-1};

    // --- Runtime, owned by the mechanic ---
    int highlighted{-1};
    int active_hand{-1};
    /** Debounce, so opening the panel with a trigger pull does not immediately
     *  spend that same pull on whatever row happens to be under the ray.
     *
     *  Note there is no ``prev_trigger`` here: ``InputSnapshot::HandState``
     *  already carries last frame's value, and a second copy of the same fact
     *  would drift the moment one was updated on a frame the other skipped. */
    float click_cooldown_s{0.35f};
    /** Clicks not yet read. Bounded: an unread queue is a caller bug, and
     *  growing without limit would turn it into a leak. */
    std::vector<UiEvent> events;
    /** Drawn above the rows (``IUi::set_title``). Empty = no heading. */
    std::string title;
    std::string subtitle;
};

/**
 * "You can act on this, right now" -- published by gameplay, drawn by the
 * renderer.
 *
 * The split matters. Whether an object is actionable depends on reach, on
 * whether it is locked, and on per-scene interaction policy, and the
 * affordance built-ins already evaluate all three every frame in order to
 * decide whether to respond. If the renderer recomputed it there would be two
 * answers to one question, and they would drift: an object would glow while
 * its mechanic refused to act on it. Advertising an interaction that will be
 * refused is worse than showing no cue at all, so the mechanic that will
 * refuse is the one that owns the cue.
 *
 * Written every frame and decayed rather than cleared, so a cue fades instead
 * of popping when a hand drifts past the edge of reach.
 */
struct CHighlight {
    /** 0 = no cue, 1 = fully highlighted. */
    float intensity = 0.0f;
    /** Rim colour. Defaults to a neutral warm highlight. */
    Vec4 color{1.0f, 0.92f, 0.62f, 1.0f};
};

/** Declared world state: what an object IS, and what about it can change.
 *
 * ``pristine`` is the value the world document declared. Everything about it
 * that survives a transition is derived by comparing ``state`` against it, so
 * a world reloads from its unchanged document plus a small set of deltas --
 * and no mechanic has to remember to write a save file. */
struct CInteractive {
    /** Stable, author-chosen, unique. The persistence key. */
    std::string id;
    /** "door", "switchable", "container", "mover", "breakable",
     *  "harvestable", "movable". */
    std::string kind;
    /** Current state, keyed by name ("open", "angle", "floor"). */
    std::unordered_map<std::string, InteractiveValue> state;
    /** The same map as it was at load. Never mutated. */
    std::unordered_map<std::string, InteractiveValue> pristine;
    /** A modifier, not a kind: a gate can be closed AND locked. */
    bool locked{false};
    /** False makes the object refuse interaction and suppresses its
     *  affordance cue. This is where a per-scene interaction policy lands --
     *  "players cannot pick grass in the hub" -- so one shared check governs
     *  both whether an action succeeds and whether it is advertised. */
    bool interaction_allowed{true};

    // --- Kind parameters, as declared. --------------------------------
    Vec3 hinge_point{0.0f, 0.0f, 0.0f};
    Vec3 hinge_axis{0.0f, 1.0f, 0.0f};
    float min_angle_deg{0.0f};
    float max_angle_deg{90.0f};
    Vec3 slide_axis{0.0f, 0.0f, 0.0f};
    float slide_distance_m{0.0f};
    std::vector<std::string> positions;
    std::string yields_asset_id;
};

/** An object whose POSITION should survive a transition.
 *
 * The other half of persistence. A door records a discrete state change; a
 * bottle the player moved from the floor to a bed records a transform. Both
 * are diffed against a load-time baseline, so neither ``builtin.grab`` nor
 * any other mechanic needs save code -- moving the object IS the save.
 *
 * ``pristine`` is where the scene document put it, captured once at load. */
struct CMovable {
    Transform pristine{};
};

/** One place something can be equipped on a wearer.
 *
 * Anatomy alone cannot express what games actually ask for -- "sword sheath
 * on the left hip", "hammer loop on the tool belt", "night-vision mount on
 * the helmet" -- so a slot is a named point defined either relative to a
 * canonical body slot or relative to a socket on ANOTHER equipped item. The
 * second case is what makes a breathing mask connect to a back-mounted
 * oxygen tank: because attachments already chain, the geometry follows for
 * free. */
struct EquipSlot {
    /** Slot name games refer to, e.g. "hand_r", "sword_sheath_l". */
    std::string name;
    /** Canonical body slot on the wearer (see ptv/api/BodySlots.h). Empty
     *  when this slot is hosted by another equipped item instead. */
    std::string body_slot;
    /** Item currently hosting this slot, and the socket on it. Both empty for
     *  an ordinary body slot. */
    std::string host_item_slot;
    std::string host_socket;
    Vec3 local_position{0.0f, 0.0f, 0.0f};
    Quat local_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    /** How close a released object has to be to snap in, in metres. 0 means
     *  proximity is not required (menu- or script-driven equipping). */
    float snap_radius_m{0.0f};
    /** Only one item at a time. Equipping into an occupied exclusive slot
     *  replaces the incumbent rather than stacking two objects in one place. */
    bool exclusive{true};
    /** Item tags this slot accepts. Empty accepts anything, which is how a
     *  sword is stopped from equipping to the helmet slot just because it
     *  brushed the player's head. */
    std::vector<std::string> accepts_tags;
};

/** The slots a wearer offers. Present on the player's avatar and on any NPC
 *  that can carry equipment. */
struct CEquipSlots {
    std::vector<EquipSlot> slots;
};

/** State of one equipped item.
 *
 * The three flags were deliberately split out of a single "locked" idea,
 * which conflated two unrelated questions and left equipped items with no
 * way to come off at all. */
struct CEquipped {
    std::uint32_t wearer{0xFFFFFFFFu};
    std::string slot;
    /** Brushing a hand past this must not detach it. Governs ACCIDENTAL
     *  removal only; it never blocks a deliberate pull. */
    bool passive_grab_protection{true};
    /** Whether intentional removal is allowed RIGHT NOW. Runtime-mutable,
     *  because games turn it off while flying, mid-experiment, outside an
     *  airlock, or while a curse holds -- and back on afterwards, on the same
     *  item instance. */
    bool removable{true};
    /** Whether this specific instance may ever be equipped again. Runtime-
     *  mutable: a sterile glove that has been removed is contaminated. */
    bool re_equippable{true};
    /** Outfit-set id, so one button can equip or remove several coordinated
     *  pieces while leaving unrelated accessories alone. */
    std::string group;
    /** Trim applied on top of the slot's own offset. Lets an item change
     *  pose without being unequipped -- goggles raised versus lowered. */
    Vec3 offset{0.0f, 0.0f, 0.0f};
    Quat offset_rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/** A planar mirror. Written by ``builtin.mirror_surface``, read by the
 *  renderer.
 *
 * Implemented as reflected geometry drawn straight into the output buffer
 * rather than a render-to-texture pass. On the tile-based GPU in a Quest an
 * offscreen target forces a GMEM round-trip to system memory and back, and
 * bandwidth is the bottleneck; Meta's own mobile-VR guidance says the
 * simplest mirror is a reflected copy of the geometry, and drawing geometry
 * twice is comparatively cheap there.
 *
 * The reflected set is deliberately tiny -- the avatar, what it is wearing,
 * and anything explicitly tagged -- because published Quest 3 measurements
 * put a full-scene stereo mirror at roughly 2.5-3.5 ms of frame time. */
struct CMirrorSurface {
    /** Plane normal in world space, pointing at the viewer. */
    Vec3 normal{0.0f, 0.0f, 1.0f};
    /** Reflect only while the viewer is within this range. One mirror is
     *  active at a time; the rest cost nothing. */
    float max_distance_m{6.0f};
    /** Tags whose entities appear in the reflection, in addition to the
     *  player's avatar and anything attached to it. */
    std::vector<std::string> reflect_tags;
    /** Hard cap on reflected entities, so a badly configured tag cannot
     *  quietly double the frame's draw calls. */
    int max_reflected{24};
    /** Set by the mechanic: only the nearest eligible mirror reflects. */
    bool active{false};
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
    /** Shared with the skeleton and every clone of the same rig. */
    gltf_mesh_load::ClipList clips;
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
    /** The clip whose pose the skeleton holds, or -1 before the first
     *  sample. Beyond the last LOD band a character holds its pose -- but
     *  only a pose of its CURRENT clip: one that spawned out there (a far
     *  goblin) held its bind pose, a T-pose, for as long as it lived. */
    int posed_clip{-1};
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
    /** Play a gait VARIANT clip at the current tier -- "punching" plays
     *  ``run.punching`` while running -- when the asset has one. The body
     *  still moves at the walk/run speed; only the clip changes. Empty plays
     *  the plain walk/run clip. */
    std::string gait_variant{};
    /** One-shot: launch upward at this speed (m/s) if standing on ground.
     *  The built-in clears it once consumed, plays the asset's ``jump`` clip
     *  when it has one, and lets gravity bring the body back down. Ignored by
     *  flying characters, which already move vertically through
     *  ``desired_velocity.y``. */
    float jump_velocity{0.0f};
    /** Turn only the HEAD toward this world point, clamped to +/-60 degrees
     *  of the body's facing, without turning the body. A guard watching the
     *  player walk past, while its feet stay put. */
    Vec3 look_target{0.0f};
    bool has_look_target{false};
};

struct CPhysics {
    std::string kind{"static"};
    float mass_kg{1.0f};
    float restitution{0.2f};
    float friction{0.5f};
    std::string collision_shape{"aabb"};
    /** Explicit capsule radius in metres (the player's body, sized from the
     *  avatar). 0 = derived from the capsule's height. */
    float capsule_radius_m{0.0f};
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

/** Stunned (a flashbang): NPC behaviours stand still and do not attack
 *  while ``remaining_s`` > 0. The engine counts it down. */
struct CStun {
    float remaining_s{0.0f};
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
    /** Socket on THIS entity (a hand_grip) to seat on the anchor instead of
     *  the authored offset. Resolved once after skeletons load (the hand's
     *  grip frame comes from the rig), then cleared. */
    std::string child_socket;
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
