#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ptv/math/Math.h"

namespace ptv::gltf_mesh_load {

/** Hard ceiling on joints per skin.
 *
 * ``SkinnedVertex::joints`` is four bytes, so index 255 is the largest value
 * that survives the GPU upload. Typical humanoid skeletons land near 65 and
 * an OpenXR hand is 26, so this is generous -- but a skin that exceeds it must
 * fail loudly instead of wrapping around and skinning to the wrong bone. */
inline constexpr int kMaxJointsPerSkin = 256;

/** Vertex layout for skinned geometry: 48 bytes.
 *
 * The first 40 bytes are byte-identical to ``LitVertex`` so both pipelines
 * can share ``scene.frag`` and the same attribute locations 0-2. Joints and
 * weights are packed to one byte each rather than widened to shorts/floats
 * because Quest 3 is memory-bandwidth bound and static meshes -- the vast
 * majority of any scene -- keep their 40-byte layout entirely. */
struct SkinnedVertex {
    float pos[3];
    float normal[3];
    float color[4];
    /** Joint indices into the skin's joint array. */
    std::uint8_t joints[4];
    /** Influence weights, quantized so the four sum to exactly 255. */
    std::uint8_t weights[4];
};
static_assert(sizeof(SkinnedVertex) == 48,
              "SkinnedVertex must stay tightly packed at 48 bytes; the Vulkan "
              "vertex input description depends on this layout");

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
    std::vector<AnimationClip> clips;

    bool empty() const { return parents.empty(); }
    size_t joint_count() const { return parents.size(); }
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
                       std::string& err);

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
                      std::string& err);

}  // namespace ptv::gltf_mesh_load
