#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/math/Math.h"
#include "ptv/render/GltfMeshLoad.h"

namespace ptv::gltf_mesh_load {

/** Hard ceiling on joints per skin.
 *
 * ``SkinnedVertex::joints`` is four bytes, so index 255 is the largest value
 * that survives the GPU upload. Typical humanoid skeletons land near 65 and
 * an OpenXR hand is 26, so this is generous -- but a skin that exceeds it must
 * fail loudly instead of wrapping around and skinning to the wrong bone. */
inline constexpr int kMaxJointsPerSkin = 256;

/** CPU-side skinned vertex: 56 bytes.
 *
 * The first 48 bytes are byte-identical to ``LitVertex`` (position, normal,
 * base-colour UV, colour), so tools and loaders treat both alike. Joints and
 * weights are one byte each. The GPU layout is packed separately (32 bytes,
 * see Renderer), because Quest 3 is memory-bandwidth bound. */
struct SkinnedVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    float color[4];
    /** Joint indices into the skin's joint array. */
    std::uint8_t joints[4];
    /** Influence weights, quantized so the four sum to exactly 255. */
    std::uint8_t weights[4];
};
static_assert(sizeof(SkinnedVertex) == 56,
              "SkinnedVertex must stay tightly packed at 56 bytes; the "
              "decimator welds whole vertices by their bytes");

/** One joint's local transform. Scale is a full vec3 rather than the uniform
 *  float in ``ptv::Transform`` because glTF permits non-uniform joint scale. */
struct JointPose {
    Vec3 translation{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};

    Mat4 matrix() const {
        Mat4 t = glm::translate(Mat4(1.0f), translation);
        Mat4 r = glm::mat4_cast(rotation);
        Mat4 s = glm::scale(Mat4(1.0f), scale);
        return t * r * s;
    }
};

enum class AnimInterp { Step, Linear, CubicSpline };
enum class AnimPath { Translation, Rotation, Scale };

/** One animated property of one joint over time.
 *
 * ``values`` is flat: 3 floats per key for translation/scale, 4 for rotation.
 * CubicSpline triples that, storing in-tangent, value, out-tangent per key. */
struct AnimChannel {
    int joint{-1};
    AnimPath path{AnimPath::Rotation};
    AnimInterp interp{AnimInterp::Linear};
    std::vector<float> times;
    std::vector<float> values;
};

struct AnimationClip {
    std::string name;
    float duration{0.0f};
    std::vector<AnimChannel> channels;
};

/** A character's clips, shared copy-on-write.
 *
 * Clip keyframes are the largest CPU-side data a rigged character carries,
 * and every clone of an NPC type (and the animator of each) used to hold its
 * own copy. Copies of a ClipList share one array; the first write through a
 * copy detaches it. Reads look exactly like a ``std::vector``. */
class ClipList {
public:
    ClipList() = default;
    ClipList(std::vector<AnimationClip> v)  // NOLINT: implicit by design
        : p_(std::make_shared<std::vector<AnimationClip>>(std::move(v))) {}

    std::size_t size() const { return p_ ? p_->size() : 0; }
    bool empty() const { return size() == 0; }
    // Indexing only reads, so a hot loop over a non-const animator never
    // detaches a shared list.
    const AnimationClip& operator[](std::size_t i) const { return (*p_)[i]; }
    const AnimationClip* begin() const { return p_ ? p_->data() : nullptr; }
    const AnimationClip* end() const { return p_ ? p_->data() + p_->size() : nullptr; }
    /** Mutable iteration (load time): detaches first if shared. */
    AnimationClip* begin() { return empty() ? nullptr : mut().data(); }
    AnimationClip* end() { return empty() ? nullptr : mut().data() + p_->size(); }
    void push_back(AnimationClip c) { mut().push_back(std::move(c)); }
    void clear() { p_.reset(); }
    /** True when ``other`` shares this list's storage (no copy). */
    bool shares_with(const ClipList& other) const { return p_ && p_ == other.p_; }
    long use_count() const { return p_ ? p_.use_count() : 0; }

private:
    std::vector<AnimationClip>& mut() {
        if (!p_) {
            p_ = std::make_shared<std::vector<AnimationClip>>();
        } else if (p_.use_count() > 1) {
            p_ = std::make_shared<std::vector<AnimationClip>>(*p_);
        }
        return *p_;
    }
    std::shared_ptr<std::vector<AnimationClip>> p_;
};

/** Everything needed to pose a skin, independent of its geometry.
 *
 * Joint indices match the glTF skin's ``joints`` array exactly, because
 * ``JOINTS_0`` references that ordering. Rather than reorder joints so
 * parents precede children (which would require rewriting every vertex's
 * indices), ``order`` supplies a parent-first traversal over the original
 * indices. */
struct SkeletonData {
    /** Parent joint index per joint, -1 for a root. */
    std::vector<int> parents;
    /** Joint indices in parent-first order; walk this to accumulate world
     *  matrices in a single pass. */
    std::vector<int> order;
    std::vector<JointPose> bind_pose;
    std::vector<Mat4> inverse_bind;
    std::vector<std::string> names;
    std::unordered_map<std::string, int> name_to_joint;
    /** Uniform scale the geometry loader applied to normalize the longest
     *  AABB axis to ``target_scale_meters``. The pose system conjugates each
     *  joint matrix by this so skinning stays consistent with the scaled
     *  vertex positions. */
    float unit_scale{1.0f};
    ClipList clips;
    /** Lowest bind-pose vertex, in metres in the entity's own space (after
     *  any model frame). Filled by ``load_glb_skinned`` only; lets the
     *  runtime stand a character that arrived with no measured foot offset
     *  on its real soles instead of on a half-height guess. */
    float bind_min_y{0.0f};

    bool empty() const { return parents.empty(); }
    size_t joint_count() const { return parents.size(); }
};

/** A character's measured model frame (``asset_contract`` -> ``scene.json``),
 *  in the GLB's own units.
 *
 * Character sources disagree about where the origin is and which way the
 * model faces: Tripo puts the origin at the feet and faces +X, Uthana centres
 * the hips and faces +Z. With a frame, the loader pivots the model so its
 * soles sit ``height / 2`` below the entity origin (the centre of its
 * capsule), its face points down -Z, and ``height`` -- not the longest axis --
 * maps to ``target_scale_meters``. The pivot is folded into the root joints
 * exactly like a non-joint ancestor node, so the palette, ``joint_world``,
 * sockets and attachments all see it without knowing it exists. */
struct ModelFrame {
    bool valid{false};
    /** Rotation about +Y that turns the model's forward onto -Z. */
    float yaw_offset_rad{0.0f};
    /** Bind-pose sole height and standing height, GLB units. */
    float sole_y{0.0f};
    float height{0.0f};
    /** Metres per GLB unit. 0 = ``target_scale_meters / height``. */
    float unit_scale{0.0f};

    /** The pivot in GLB units: rotate about Y, then drop the soles to
     *  ``-height / 2``. */
    Mat4 pivot() const;
    /** Metres per GLB unit for ``target_scale_meters``. */
    float scale_for(float target_scale_meters) const;
};

/** Turn a set of local joint transforms into the matrices the vertex shader
 *  consumes: ``palette[j] = jointWorld[j] * inverseBind[j]``.
 *
 * Walking ``skeleton.order`` guarantees a parent is finished before any of
 * its children, so this is a single pass with no recursion.
 *
 * The translation column is scaled by ``unit_scale`` at the end. That term
 * exists because the loader shrinks vertex positions to
 * ``target_scale_meters`` while the joint matrices are built in the source
 * file's units; conjugating an affine matrix by a uniform scale reduces to
 * exactly this. Skip it and a scaled character tears apart the moment it
 * moves.
 *
 * ``local_pose`` must have one entry per joint. Passing ``bind_pose``
 * produces an all-identity palette. */
void build_joint_palette(const SkeletonData& skeleton,
                         const std::vector<JointPose>& local_pose,
                         std::vector<Mat4>& out_palette);

/** Cheap probe: does this GLB contain a skin, and how many joints?
 *
 * Parses the JSON chunk only -- no vertex data is decoded -- so the scene
 * loader can ask this for every mesh without paying for geometry. */
bool glb_probe_skin(const std::string& path,
                    bool* out_has_skin,
                    int* out_joint_count,
                    std::string& err);

/** Load the skeleton (hierarchy, bind pose, inverse bind matrices, clips)
 *  without decoding vertex data.
 *
 * ``unit_scale`` is derived from the POSITION accessors' required min/max
 * bounds, so it matches what ``load_glb_skinned`` computes from the real
 * geometry without reading the BIN chunk. */
bool load_glb_skeleton(const std::string& path,
                       float target_scale_meters,
                       SkeletonData& out_skeleton,
                       std::string& err,
                       const ModelFrame* frame = nullptr);

/** Merge all skinned triangle primitives into one indexed buffer and load the
 *  matching skeleton.
 *
 * Vertices stay in the skin's own coordinate space: per the glTF spec the
 * transform of a skinned mesh's node MUST be ignored, since the joint
 * matrices already position the mesh. Positions are then uniformly scaled to
 * ``target_scale_meters`` on the longest axis, exactly like the static path.
 *
 * Fails when the GLB has no skin -- callers should fall back to
 * ``load_glb_merged``. */
bool load_glb_skinned(const std::string& path,
                      float target_scale_meters,
                      std::vector<SkinnedVertex>& out_verts,
                      std::vector<std::uint32_t>& out_indices,
                      SkeletonData& out_skeleton,
                      std::string& err,
                      const ModelFrame* frame = nullptr,
                      MeshMaterials* out_materials = nullptr,
                      const LoadOptions& options = {});

}  // namespace ptv::gltf_mesh_load
