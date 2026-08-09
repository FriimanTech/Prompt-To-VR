// Lightweight C++ smoke tests for the runtime.
//
// Usage: `ptv_smoke <case> [args]` where <case> is one of:
//    mechanics   — registry knows the built-ins
//    scene       — parses a trivial inline scene
//    openxr      — tries to create an XrInstance; passes even if no runtime
//    skinning <glb>      — rigged GLB loads: skin, weights, clips, bind pose
//    gltf_nodes <glb>    — static mesh inherits its node's world transform
//    skinning_math <glb> — CPU mirror of the skinning equation
//    skinning_gpu <glb>  — one real validated frame through the skinned pipeline
//    pose_system <glb>   — clip sampling + procedural joint override
//    lod_skinning <glb…> — every decimated LOD keeps the same skeleton
//    hand_asset <glb>    — the generated hand matches builtin.hand_pose's contract
//    hand_pose <glb>     — curling one finger moves only that finger
//    grab_socket         — a grip socket lands in the hand from any approach
//    clip_contract       — animation intents resolve to real glTF clip names

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ptv/ecs/Skeleton.h"
#include "ptv/ecs/World.h"
#include "ptv/mechanics/MechanicRegistry.h"
#include "ptv/render/GltfMeshLoad.h"
#include "ptv/render/GltfSkinnedLoad.h"
#include "ptv/render/Renderer.h"
#include "ptv/render/VulkanContext.h"
#include "ptv/scene/SceneLoader.h"

#include <openxr/openxr.h>

static int ok(const char* what) {
    std::printf("[ok] %s\n", what);
    return 0;
}

static int case_mechanics() {
    const auto ids = ptv::MechanicRegistry::get().mechanic_ids();
    const char* expected[] = {
        "builtin.teleport",
        "builtin.locomotion.teleport",
        "builtin.locomotion.slide.head_relative",
        "builtin.locomotion.slide.controller_relative",
        "builtin.snap_turn",
        "builtin.locomotion.snap_turn",
        "builtin.grab",
        "builtin.throw",
        "builtin.recoil",
        "builtin.hand_pose",
    };
    for (const char* id : expected) {
        bool found = false;
        for (auto& got : ids)
            if (got == id) {
                found = true;
                break;
            }
        if (!found) {
            std::fprintf(stderr, "[fail] missing builtin: %s\n", id);
            return 1;
        }
    }
    return ok("mechanics: all built-ins present");
}

static int case_scene() {
    // A small, self-contained valid scene JSON document.
    const char* doc = R"({
        "version": 1, "prompt_id": "t", "scene_id": "s", "title": "T", "name": "S",
        "player": {"height_meters": 1.7, "locomotion": "teleport", "comfort": "comfort",
                    "spawn": {"position": [0,0,0], "rotation": [0,0,0,1]}},
        "sky_color": [0,0,0], "ambient_intensity": 0.1,
        "lights": [],
        "entities": [
          {"id": "cube", "name": "Cube",
           "transform": {"position": [0,1,-1], "rotation": [0,0,0,1], "scale": 1.0},
           "mesh": {"asset_id": "cube", "glb_path": "missing.glb",
                    "target_scale_meters": 0.2, "collision_shape": "aabb",
                    "sockets": {
                      "muzzle": {"position": [0, 0.02, -0.1],
                                 "rotation": [0, 0, 0, 1]}}},
           "physics": {"kind": "dynamic", "mass_kg": 0.5, "restitution": 0.2,
                        "friction": 0.5, "collision_shape": "aabb"},
           "mechanic_bindings": [{"id": "builtin.grab", "builtin_id": "builtin.grab",
                                  "params": {}}],
           "tags": []}
        ]
    })";
    // Write to a temp file so LoadSceneJson's ifstream works.
    const char* path = "ptv_smoke_scene.json";
    std::FILE* f = std::fopen(path, "wb");
    std::fwrite(doc, 1, std::strlen(doc), f);
    std::fclose(f);
    auto loaded = ptv::LoadSceneJson(path);
    if (loaded.entities.size() != 1) {
        std::fprintf(stderr, "[fail] scene load produced %zu entities\n",
                     loaded.entities.size());
        return 1;
    }
    // The socket must survive parse -> SceneDoc -> ECS. This is the contract
    // ctx.sockets reads at runtime, so a silent drop anywhere along the chain
    // would surface only as projectiles leaving from the wrong place.
    if (!loaded.entities[0].mesh.has_value() ||
        loaded.entities[0].mesh->sockets.count("muzzle") != 1) {
        std::fprintf(stderr, "[fail] scene load lost the 'muzzle' socket\n");
        return 1;
    }
    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);

    const auto e = world.find("cube");
    if (e == entt::null || !world.registry().all_of<ptv::CMesh>(e)) {
        std::fprintf(stderr, "[fail] cube entity missing CMesh\n");
        return 1;
    }
    const auto& sockets = world.registry().get<ptv::CMesh>(e).sockets;
    const auto it = sockets.find("muzzle");
    if (it == sockets.end() || std::fabs(it->second.position.z + 0.1f) > 1e-5f) {
        std::fprintf(stderr, "[fail] muzzle socket did not reach CMesh\n");
        return 1;
    }
    return ok("scene: 1 entity + muzzle socket applied into ECS");
}

static int fail(const char* what) {
    std::fprintf(stderr, "[fail] %s\n", what);
    return 1;
}

// Loads the fixture written by engine/tools/make_rigged_fixture.py and checks
// every hand-off the skinning tier depends on: the skin is detected without
// decoding geometry, the hierarchy resolves parent-first, inverse bind
// matrices survive, weights quantize to an exact partition of unity, and all
// three glTF interpolation modes round-trip.
static int case_skinning(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    // --- cheap probe (JSON chunk only) ----------------------------------
    bool has_skin = false;
    int probe_joints = 0;
    if (!gml::glb_probe_skin(glb, &has_skin, &probe_joints, err)) {
        std::fprintf(stderr, "[fail] glb_probe_skin: %s\n", err.c_str());
        return 1;
    }
    if (!has_skin) return fail("probe did not detect a skin");
    if (probe_joints != 2) return fail("probe reported the wrong joint count");

    // --- full skinned load ----------------------------------------------
    std::vector<gml::SkinnedVertex> verts;
    std::vector<std::uint32_t> indices;
    gml::SkeletonData skel;
    if (!gml::load_glb_skinned(glb, 0.0f, verts, indices, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skinned: %s\n", err.c_str());
        return 1;
    }
    if (skel.joint_count() != 2) return fail("expected 2 joints");
    if (skel.parents[0] != -1 || skel.parents[1] != 0)
        return fail("joint parent chain is wrong");
    if (skel.order.size() != 2 || skel.order[0] != 0)
        return fail("traversal order is not parent-first");
    if (skel.name_to_joint.count("bone_root") != 1 ||
        skel.name_to_joint.count("bone_tip") != 1) {
        return fail("joint names did not survive the load");
    }
    // bone_tip binds at y = 1, so its inverse bind matrix must undo that.
    if (std::fabs(skel.inverse_bind[1][3][1] + 1.0f) > 1e-5f)
        return fail("inverse bind matrix for bone_tip is wrong");

    // Bind pose must reproduce identity palettes; that is what makes a rigged
    // mesh drawn un-posed match the same mesh loaded as static geometry.
    for (int j : skel.order) {
        ptv::Mat4 world = skel.bind_pose[static_cast<size_t>(j)].matrix();
        if (skel.parents[static_cast<size_t>(j)] >= 0) {
            world = skel.bind_pose[static_cast<size_t>(
                                       skel.parents[static_cast<size_t>(j)])]
                        .matrix() *
                    world;
        }
        const ptv::Mat4 palette =
            world * skel.inverse_bind[static_cast<size_t>(j)];
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                const float want = (c == r) ? 1.0f : 0.0f;
                if (std::fabs(palette[c][r] - want) > 1e-4f)
                    return fail("bind-pose palette is not the identity");
            }
        }
    }

    // --- per-vertex bindings --------------------------------------------
    bool saw_blend = false;
    for (const auto& v : verts) {
        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            if (v.joints[k] >= skel.joint_count() && v.weights[k] != 0)
                return fail("vertex references a joint outside the skin");
            sum += v.weights[k];
        }
        if (sum != 255) return fail("quantized weights do not sum to 255");
        if (v.weights[0] != 0 && v.weights[1] != 0) saw_blend = true;
    }
    if (!saw_blend) return fail("no vertex blends both joints");

    // --- animation clips -------------------------------------------------
    if (skel.clips.size() != 1) return fail("expected exactly 1 clip");
    const auto& clip = skel.clips[0];
    if (clip.channels.size() != 3) return fail("expected 3 animation channels");
    bool step = false, linear = false, cubic = false;
    for (const auto& ch : clip.channels) {
        step |= ch.interp == gml::AnimInterp::Step;
        linear |= ch.interp == gml::AnimInterp::Linear;
        cubic |= ch.interp == gml::AnimInterp::CubicSpline;
    }
    if (!step || !linear || !cubic)
        return fail("not all three interpolation modes parsed");
    if (std::fabs(clip.duration - 1.0f) > 1e-5f)
        return fail("clip duration is wrong");

    // --- static path agreement -------------------------------------------
    // load_glb_merged must drop the skinned node's transform exactly like the
    // skinned path does, so the two agree vertex-for-vertex in bind pose.
    std::vector<gml::LitVertex> static_verts;
    std::vector<std::uint32_t> static_indices;
    if (!gml::load_glb_merged(glb, 0.0f, static_verts, static_indices, err)) {
        std::fprintf(stderr, "[fail] load_glb_merged: %s\n", err.c_str());
        return 1;
    }
    if (static_verts.size() != verts.size() ||
        static_indices.size() != indices.size()) {
        return fail("static and skinned loads disagree on geometry size");
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(static_verts[i].pos[k] - verts[i].pos[k]) > 1e-6f)
                return fail("static and skinned bind poses differ");
        }
    }

    std::printf(
        "[ok] skinning: %zu verts, %zu tris, %zu joints, %zu clip(s); bind "
        "pose matches the static load\n",
        verts.size(), indices.size() / 3, skel.joint_count(),
        skel.clips.size());
    return 0;
}

// The mirror image of case_skinning's last check: a STATIC mesh must inherit
// its node's world transform. This is not academic -- raw Tripo GLBs ship a
// node matrix carrying scale and a yaw, and the loader used to drop it, so
// every optimized LOD and every derived socket was baked in the wrong
// orientation. The fixture's pivot is a 90-degree yaw plus a translation,
// both recoverable from a single vertex.
static int case_gltf_nodes(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    std::vector<gml::LitVertex> verts;
    std::vector<std::uint32_t> indices;
    // target_scale_meters = 0 keeps the source coordinate system, so the
    // expected numbers below are exact rather than renormalized.
    if (!gml::load_glb_merged(glb, 0.0f, verts, indices, err)) {
        std::fprintf(stderr, "[fail] load_glb_merged: %s\n", err.c_str());
        return 1;
    }
    if (verts.empty()) return fail("static fixture loaded no vertices");

    // Ring 0 vertex 0 is authored at (0.25, 0, 0). A +90 degree yaw maps
    // (x, y, z) -> (z, y, -x), giving (0, 0, -0.25); the pivot then adds
    // (1, 0.5, 0).
    const float want[3] = {1.0f, 0.5f, -0.25f};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(verts[0].pos[k] - want[k]) > 1e-5f) {
            std::fprintf(stderr,
                         "[fail] node transform not applied: got (%.4f, %.4f, "
                         "%.4f), want (%.4f, %.4f, %.4f)\n",
                         verts[0].pos[0], verts[0].pos[1], verts[0].pos[2],
                         want[0], want[1], want[2]);
            return 1;
        }
    }

    // The normal must ride the rotation too, or lighting flips on any asset
    // whose node carries a yaw.
    const float want_n[3] = {0.0f, 0.0f, -1.0f};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(verts[0].normal[k] - want_n[k]) > 1e-5f) {
            std::fprintf(stderr,
                         "[fail] normal not rotated: got (%.4f, %.4f, %.4f)\n",
                         verts[0].normal[0], verts[0].normal[1],
                         verts[0].normal[2]);
            return 1;
        }
    }
    return ok("gltf nodes: static mesh inherits its node world transform");
}

// Mirrors, on the CPU, exactly what scene_skinned.vert computes, and asserts
// the two properties the GPU path is built on:
//
//   1. A bind-pose palette is the identity, so a rigged mesh drawn un-posed
//      lands precisely where the static pipeline would put it.
//   2. Rotating one joint moves ONLY the vertices weighted to it. That is the
//      property a joint-index or weight-decoding bug destroys, and the one
//      that is nearly impossible to eyeball in a headset.
static int case_skinning_math(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    std::vector<gml::SkinnedVertex> verts;
    std::vector<std::uint32_t> indices;
    gml::SkeletonData skel;
    if (!gml::load_glb_skinned(glb, 0.0f, verts, indices, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skinned: %s\n", err.c_str());
        return 1;
    }

    // The shader's skinning equation, evaluated on the CPU.
    auto skin_vertex = [](const gml::SkinnedVertex& v,
                          const std::vector<ptv::Mat4>& palette) {
        ptv::Mat4 m(0.0f);
        for (int k = 0; k < 4; ++k) {
            const float w = static_cast<float>(v.weights[k]) / 255.0f;
            if (w == 0.0f) continue;
            m += w * palette[v.joints[k]];
        }
        return ptv::Vec3(m * ptv::Vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f));
    };

    std::vector<ptv::Mat4> palette;
    gml::build_joint_palette(skel, skel.bind_pose, palette);

    for (size_t i = 0; i < verts.size(); ++i) {
        const ptv::Vec3 bind(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        if (glm::length(skin_vertex(verts[i], palette) - bind) > 1e-4f) {
            std::fprintf(stderr,
                         "[fail] bind pose moved vertex %zu\n", i);
            return 1;
        }
    }

    // Now bend bone_tip 45 degrees about Z and re-derive the palette.
    const int tip = skel.name_to_joint.at("bone_tip");
    std::vector<gml::JointPose> pose = skel.bind_pose;
    pose[static_cast<size_t>(tip)].rotation =
        glm::angleAxis(glm::radians(45.0f), ptv::Vec3(0.0f, 0.0f, 1.0f)) *
        pose[static_cast<size_t>(tip)].rotation;
    gml::build_joint_palette(skel, pose, palette);

    size_t moved = 0;
    float max_shift = 0.0f;
    for (size_t i = 0; i < verts.size(); ++i) {
        const ptv::Vec3 bind(verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]);
        const float shift = glm::length(skin_vertex(verts[i], palette) - bind);

        // Weight index 1 is bone_tip in this fixture (see the generator).
        const bool tip_weighted = verts[i].weights[1] != 0;
        if (!tip_weighted && shift > 1e-4f) {
            std::fprintf(stderr,
                         "[fail] vertex %zu has no bone_tip weight but moved "
                         "%.5f\n",
                         i, shift);
            return 1;
        }
        if (tip_weighted && shift > 1e-4f) ++moved;
        max_shift = std::max(max_shift, shift);
    }
    if (moved == 0) return fail("no tip-weighted vertex moved");
    // The top ring sits a full metre from the pivot, so a 45-degree bend has
    // to displace it by roughly 2*sin(22.5deg) ~= 0.765 m.
    if (max_shift < 0.5f) {
        std::fprintf(stderr, "[fail] largest shift was only %.4f m\n",
                     max_shift);
        return 1;
    }

    std::printf(
        "[ok] skinning math: bind pose is identity; %zu/%zu vertices follow "
        "bone_tip, max shift %.3f m, unweighted vertices unmoved\n",
        moved, verts.size(), max_shift);
    return 0;
}

// Renders one frame of the rigged fixture on a real device with the
// validation layers on. This is what proves the pieces the CPU test cannot
// reach: the 48-byte vertex input description, the storage-buffer binding,
// and the second pipeline actually agreeing with each other.
static int case_skinning_gpu(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;

    ptv::VulkanContext vk;
    ptv::VulkanContext::Config cfg{};
    cfg.app_name = "ptv_smoke_skinning";
    cfg.enable_validation = true;
    if (!vk.initialize(cfg)) {
        std::printf("[info] no Vulkan device available (ok in headless CI)\n");
        return 0;
    }
    ptv::VulkanContext::reset_validation_error_count();

    constexpr uint32_t kSize = 128;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    ptv::Renderer renderer;
    if (!renderer.initialize(vk, fmt, kSize, kSize)) {
        return fail("Renderer::initialize failed");
    }

    gml::SkeletonData skel;
    const int slot = renderer.load_glb_skinned(glb, 0.0f, skel);
    if (slot < 0) return fail("renderer could not load the rigged fixture");
    if (!renderer.mesh_slot_is_skinned(slot))
        return fail("mesh slot was not marked skinned");

    // Two offscreen colour targets standing in for the XR swapchain images.
    std::array<VkImage, 2> images{VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::array<VmaAllocation, 2> allocs{nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = {kSize, kSize, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(vk.allocator(), &ici, &aci, &images[i], &allocs[i],
                           nullptr) != VK_SUCCESS) {
            return fail("could not allocate an offscreen eye image");
        }
    }

    std::array<ptv::Renderer::EyeTarget, 2> eyes{};
    for (int i = 0; i < 2; ++i) {
        eyes[i].image = images[i];
        eyes[i].width = kSize;
        eyes[i].height = kSize;
        eyes[i].view = glm::lookAt(ptv::Vec3(0.0f, 1.0f, 4.0f),
                                   ptv::Vec3(0.0f, 1.0f, 0.0f),
                                   ptv::Vec3(0.0f, 1.0f, 0.0f));
        eyes[i].projection = glm::perspective(glm::radians(70.0f), 1.0f, 0.05f,
                                              100.0f);
    }

    // One bind-pose draw, then one bent-pose draw, so both the identity and a
    // non-trivial palette go through the real shader.
    std::vector<ptv::Mat4> palette;
    std::vector<gml::JointPose> pose = skel.bind_pose;
    const int tip = skel.name_to_joint.at("bone_tip");

    for (int frame = 0; frame < 2; ++frame) {
        if (frame == 1) {
            pose[static_cast<size_t>(tip)].rotation =
                glm::angleAxis(glm::radians(30.0f),
                               ptv::Vec3(0.0f, 0.0f, 1.0f));
        }
        gml::build_joint_palette(skel, pose, palette);

        renderer.begin_joint_palettes();
        const int base = renderer.push_joint_palette(
            palette.data(), static_cast<int>(palette.size()));
        if (base < 0) return fail("push_joint_palette rejected the palette");

        ptv::Renderer::DrawItem item{};
        item.model = ptv::Mat4(1.0f);
        item.mesh_slot = slot;
        item.joint_base = base;
        item.joint_count = static_cast<int>(palette.size());

        std::vector<ptv::Renderer::DrawItem> items{item};
        ptv::Renderer::RenderEnv env{};
        renderer.draw_frame(eyes, items, env);
    }

    const std::uint32_t errors =
        ptv::VulkanContext::validation_error_count();

    renderer.shutdown();
    for (int i = 0; i < 2; ++i) {
        if (images[i]) vmaDestroyImage(vk.allocator(), images[i], allocs[i]);
    }
    vk.shutdown();

    if (errors != 0) {
        std::fprintf(stderr,
                     "[fail] %u Vulkan validation error(s) during the skinned "
                     "draw\n",
                     errors);
        return 1;
    }
    return ok("skinning gpu: bind-pose and bent-pose frames drew clean");
}

// Drives CSkeleton / CAnimator through the real ECS tick and checks the two
// behaviours that make clip playback and procedural posing coexist:
//
//   1. A clip evaluated at a known time produces the pose the file asked for,
//      across all three interpolation modes.
//   2. A joint a mechanic overrode this frame keeps the override even while
//      the clip is running, and is handed back to the clip the frame after
//      the mechanic stops writing it.
static int case_pose_system(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    gml::SkeletonData skel;
    if (!gml::load_glb_skeleton(glb, 0.0f, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skeleton: %s\n", err.c_str());
        return 1;
    }
    if (skel.clips.empty()) return fail("fixture carried no clips");

    ptv::World world;
    const auto e = world.create_entity("rig");
    const auto id = static_cast<std::uint32_t>(e);

    ptv::CSkeleton cs{};
    cs.local_pose = skel.bind_pose;
    cs.skeleton = skel;
    world.registry().emplace<ptv::CSkeleton>(e, std::move(cs));

    ptv::CAnimator anim{};
    anim.clips = skel.clips;
    anim.active_clip = 0;
    anim.looping = false;
    anim.playing = true;
    anim.joint_overridden.assign(skel.joint_count(), 0u);
    world.registry().emplace<ptv::CAnimator>(e, std::move(anim));

    const int tip = skel.name_to_joint.at("bone_tip");
    const int root = skel.name_to_joint.at("bone_root");

    // --- clip playback ---------------------------------------------------
    // The fixture's LINEAR rotation track peaks at t = 0.5 s with a 45-degree
    // turn about Z, so advancing exactly half a second must land there.
    ptv::tick_animators(world, 0.5f);
    {
        const auto& sk = world.get<ptv::CSkeleton>(id);
        const ptv::Quat q = sk.local_pose[static_cast<size_t>(tip)].rotation;
        const float angle = glm::degrees(2.0f * std::acos(
            std::min(1.0f, std::fabs(q.w))));
        if (std::fabs(angle - 45.0f) > 1.0f) {
            std::fprintf(stderr,
                         "[fail] clip at t=0.5 gave %.2f degrees, want 45\n",
                         angle);
            return 1;
        }
        // The STEP translation track holds 0.1 on X from t = 0.5 onward.
        const ptv::Vec3 t =
            sk.local_pose[static_cast<size_t>(root)].translation;
        if (std::fabs(t.x - 0.1f) > 1e-4f) {
            std::fprintf(stderr,
                         "[fail] STEP track gave x=%.4f, want 0.1\n", t.x);
            return 1;
        }
        // CUBICSPLINE scale runs 1.0 -> 1.2 on Y; halfway must be strictly
        // between, which a broken Hermite basis would not produce.
        const float sy = sk.local_pose[static_cast<size_t>(root)].scale.y;
        if (sy <= 1.0f || sy >= 1.2f) {
            std::fprintf(stderr,
                         "[fail] CUBICSPLINE scale.y=%.4f, want strictly "
                         "between 1.0 and 1.2\n",
                         sy);
            return 1;
        }
        if (sk.palette.size() != skel.joint_count())
            return fail("palette was not rebuilt");
    }

    // --- procedural override beats the clip ------------------------------
    const ptv::Quat forced =
        glm::angleAxis(glm::radians(-20.0f), ptv::Vec3(0.0f, 0.0f, 1.0f));
    {
        auto& sk = world.get<ptv::CSkeleton>(id);
        auto& an = world.get<ptv::CAnimator>(id);
        sk.local_pose[static_cast<size_t>(tip)].rotation = forced;
        an.joint_overridden[static_cast<size_t>(tip)] = 1u;
    }
    ptv::tick_animators(world, 0.1f);
    {
        const auto& sk = world.get<ptv::CSkeleton>(id);
        const ptv::Quat q = sk.local_pose[static_cast<size_t>(tip)].rotation;
        if (std::fabs(glm::dot(q, forced)) < 0.9999f)
            return fail("clip overwrote an overridden joint");
        // ...while a joint nobody overrode still follows the clip.
        const ptv::Vec3 t =
            sk.local_pose[static_cast<size_t>(root)].translation;
        if (std::fabs(t.x - 0.1f) > 1e-4f)
            return fail("override leaked onto a non-overridden joint");
    }

    // --- releasing the override hands the joint back ---------------------
    ptv::tick_animators(world, 0.1f);
    {
        const auto& sk = world.get<ptv::CSkeleton>(id);
        const ptv::Quat q = sk.local_pose[static_cast<size_t>(tip)].rotation;
        if (std::fabs(glm::dot(q, forced)) > 0.9999f)
            return fail("override stuck after the mechanic stopped writing");
    }

    return ok("pose system: clip sampling, override priority, and release");
}

// Checks the invariant the whole LOD-under-animation scheme rests on: every
// level of a rigged asset must expose the SAME skeleton. If a coarser LOD
// renumbered its joints or shifted an inverse bind matrix, one palette could
// not serve all of them, and a distance-driven LOD switch would snap the
// character into a garbage pose mid-animation.
static int case_lod_skinning(int count, char** paths) {
    namespace gml = ptv::gltf_mesh_load;

    gml::SkeletonData reference;
    std::size_t prev_tris = 0;

    for (int i = 0; i < count; ++i) {
        std::vector<gml::SkinnedVertex> verts;
        std::vector<std::uint32_t> indices;
        gml::SkeletonData skel;
        std::string err;
        if (!gml::load_glb_skinned(paths[i], 0.0f, verts, indices, skel,
                                   err)) {
            std::fprintf(stderr, "[fail] LOD %d (%s): %s\n", i, paths[i],
                         err.c_str());
            return 1;
        }

        // Bindings must survive decimation intact.
        for (const auto& v : verts) {
            int sum = 0;
            for (int k = 0; k < 4; ++k) {
                if (v.joints[k] >= skel.joint_count() && v.weights[k] != 0) {
                    std::fprintf(stderr,
                                 "[fail] LOD %d references joint %u of %zu\n",
                                 i, static_cast<unsigned>(v.joints[k]),
                                 skel.joint_count());
                    return 1;
                }
                sum += v.weights[k];
            }
            if (sum != 255) {
                std::fprintf(stderr,
                             "[fail] LOD %d has weights summing to %d\n", i,
                             sum);
                return 1;
            }
        }

        // Animation clips must survive decimation. They target NODES, so
        // nothing about simplifying the mesh can invalidate them -- but the
        // optimizer has to actually write them back out, and for a while it
        // did not, which silently left every LOD un-animatable.
        if (skel.clips.empty())
            return fail("a LOD lost its animation clips entirely");

        if (i == 0) {
            reference = skel;
        } else {
            if (skel.joint_count() != reference.joint_count())
                return fail("LOD joint counts differ");
            if (skel.parents != reference.parents)
                return fail("LOD joint hierarchies differ");
            if (skel.order != reference.order)
                return fail("LOD traversal orders differ");
            if (skel.names != reference.names)
                return fail("LOD joint names differ");
            for (size_t j = 0; j < reference.joint_count(); ++j) {
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        if (std::fabs(skel.inverse_bind[j][c][r] -
                                      reference.inverse_bind[j][c][r]) >
                            1e-6f) {
                            return fail("LOD inverse bind matrices differ");
                        }
                    }
                }
            }

            if (skel.clips.size() != reference.clips.size())
                return fail("LOD clip counts differ");
            for (size_t c = 0; c < reference.clips.size(); ++c) {
                const auto& got = skel.clips[c];
                const auto& want = reference.clips[c];
                // The NAME is the contract ISkeleton::play_clip matches on,
                // so a rename between levels would break a mechanic the
                // moment the player walked far enough to switch LOD.
                if (got.name != want.name)
                    return fail("LOD clip names differ");
                if (std::fabs(got.duration - want.duration) > 1e-5f)
                    return fail("LOD clip durations differ");
                if (got.channels.size() != want.channels.size())
                    return fail("LOD clip channel counts differ");
                for (size_t ch = 0; ch < want.channels.size(); ++ch) {
                    const auto& a = got.channels[ch];
                    const auto& b = want.channels[ch];
                    if (a.joint != b.joint || a.path != b.path ||
                        a.interp != b.interp) {
                        return fail("LOD clip channel targets differ");
                    }
                    if (a.times != b.times || a.values != b.values)
                        return fail("LOD clip keyframe data differs");
                }
            }
        }

        const std::size_t tris = indices.size() / 3;
        if (i > 0 && tris > prev_tris) {
            std::fprintf(stderr,
                         "[fail] LOD %d has %zu tris, more than LOD %d's %zu\n",
                         i, tris, i - 1, prev_tris);
            return 1;
        }
        prev_tris = tris;
        std::printf("       LOD %d: %zu tris, %zu verts, %zu joints, %zu clip(s)\n",
                    i, tris, verts.size(), skel.joint_count(),
                    skel.clips.size());
    }

    return ok("lod skinning: identical skeleton, bindings and clips at every "
              "level");
}

// The generated hand asset is what makes builtin.hand_pose possible, so it
// has a contract the mechanic depends on by name: 16 joints, mixamorig:
// bone names (hand fixture only; Tripo NPCs use spec=tripo), and knuckle
// vertices that blend two joints (a hand rigidly bound one-box-per-joint
// would tear open at every bend).
static int case_hand_asset(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    std::vector<gml::SkinnedVertex> verts;
    std::vector<std::uint32_t> indices;
    gml::SkeletonData skel;
    if (!gml::load_glb_skinned(glb, 0.0f, verts, indices, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skinned: %s\n", err.c_str());
        return 1;
    }

    if (skel.joint_count() != 16) {
        std::fprintf(stderr, "[fail] hand has %zu joints, want 16\n",
                     skel.joint_count());
        return 1;
    }
    if (skel.name_to_joint.count("mixamorig:hand_root") != 1)
        return fail("hand has no mixamorig:hand_root");

    // Exactly the names builtin.hand_pose resolves. A rename here silently
    // turns the whole mechanic into a no-op, so assert the contract.
    const char* fingers[5] = {"thumb", "index", "middle", "ring", "pinky"};
    for (const char* finger : fingers) {
        for (int s = 1; s <= 3; ++s) {
            const std::string name =
                std::string("mixamorig:") + finger + "_0" + std::to_string(s);
            if (skel.name_to_joint.count(name) != 1) {
                std::fprintf(stderr, "[fail] hand is missing joint %s\n",
                             name.c_str());
                return 1;
            }
        }
    }

    // Every finger joint must hang off the wrist chain, or curling a knuckle
    // would not carry the segments beyond it.
    const int root = skel.name_to_joint.at("mixamorig:hand_root");
    for (const char* finger : fingers) {
        const int proximal =
            skel.name_to_joint.at(std::string("mixamorig:") + finger + "_01");
        if (skel.parents[static_cast<size_t>(proximal)] != root) {
            std::fprintf(stderr, "[fail] %s_01 is not parented to the wrist\n",
                         finger);
            return 1;
        }
    }

    size_t blended = 0;
    for (const auto& v : verts) {
        int sum = 0;
        for (int k = 0; k < 4; ++k) {
            if (v.joints[k] >= skel.joint_count() && v.weights[k] != 0)
                return fail("hand vertex references a joint outside the skin");
            sum += v.weights[k];
        }
        if (sum != 255) return fail("hand weights do not sum to 255");
        if (v.weights[0] != 0 && v.weights[1] != 0) ++blended;
    }
    if (blended == 0)
        return fail("no knuckle vertices blend two joints; bends would tear");

    std::printf(
        "[ok] hand asset: %zu verts, %zu tris, %zu joints, %zu blended "
        "knuckle vertices\n",
        verts.size(), indices.size() / 3, skel.joint_count(), blended);
    return 0;
}

// Mirrors what builtin.hand_pose does to the skeleton and asserts the two
// properties that make a curl read as a hand: the right fingers move for the
// right control, and nothing else does.
static int case_hand_pose(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    gml::SkeletonData skel;
    if (!gml::load_glb_skeleton(glb, 0.0f, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skeleton: %s\n", err.c_str());
        return 1;
    }

    auto joint = [&](const char* finger, int seg) {
        return skel.name_to_joint.at(std::string("mixamorig:") + finger + "_0" +
                                     std::to_string(seg));
    };

    // Tip of each finger in bind pose, so we can see which ones moved.
    auto tip_position = [&](const char* finger,
                            const std::vector<ptv::Mat4>& palette) {
        const int j = joint(finger, 3);
        // The palette maps bind-space to posed space, so applying it to the
        // joint's own bind-space origin gives where the tip ended up.
        ptv::Mat4 world(1.0f);
        int cur = j;
        ptv::Vec3 bind(0.0f);
        while (cur >= 0) {
            bind += skel.bind_pose[static_cast<size_t>(cur)].translation;
            cur = skel.parents[static_cast<size_t>(cur)];
        }
        (void)world;
        return ptv::Vec3(palette[static_cast<size_t>(j)] *
                         ptv::Vec4(bind, 1.0f));
    };

    std::vector<ptv::Mat4> bind_palette;
    gml::build_joint_palette(skel, skel.bind_pose, bind_palette);

    // Curl ONLY the index finger, the way a full trigger pull would.
    std::vector<gml::JointPose> pose = skel.bind_pose;
    for (int s = 1; s <= 3; ++s) {
        const int j = joint("index", s);
        pose[static_cast<size_t>(j)].rotation =
            glm::angleAxis(1.2f, ptv::Vec3(1.0f, 0.0f, 0.0f));
    }
    std::vector<ptv::Mat4> posed;
    gml::build_joint_palette(skel, pose, posed);

    const float index_shift = glm::length(tip_position("index", posed) -
                                          tip_position("index", bind_palette));
    if (index_shift < 0.02f) {
        std::fprintf(stderr,
                     "[fail] index tip moved only %.4f m on a full curl\n",
                     index_shift);
        return 1;
    }
    for (const char* other : {"middle", "ring", "pinky", "thumb"}) {
        const float shift = glm::length(tip_position(other, posed) -
                                        tip_position(other, bind_palette));
        if (shift > 1e-5f) {
            std::fprintf(stderr,
                         "[fail] curling the index moved %s by %.5f m\n",
                         other, shift);
            return 1;
        }
    }

    // A resting touch has to land strictly between open and closed, or
    // capsense buys nothing over the plain analog value.
    std::vector<gml::JointPose> resting = skel.bind_pose;
    for (int s = 1; s <= 3; ++s) {
        const int j = joint("index", s);
        resting[static_cast<size_t>(j)].rotation =
            glm::angleAxis(0.18f * 1.2f, ptv::Vec3(1.0f, 0.0f, 0.0f));
    }
    std::vector<ptv::Mat4> resting_palette;
    gml::build_joint_palette(skel, resting, resting_palette);
    const float rest_shift =
        glm::length(tip_position("index", resting_palette) -
                    tip_position("index", bind_palette));
    if (!(rest_shift > 1e-4f && rest_shift < index_shift)) {
        std::fprintf(stderr,
                     "[fail] resting curl %.4f m is not between 0 and the "
                     "full curl %.4f m\n",
                     rest_shift, index_shift);
        return 1;
    }

    std::printf(
        "[ok] hand pose: index tip travels %.3f m at full curl, %.3f m at "
        "rest, other fingers unmoved\n",
        index_shift, rest_shift);
    return 0;
}

// builtin.grab's grip-socket alignment, evaluated with the same maths the
// mechanic uses. The point of the feature is that the result does NOT depend
// on how the player reached for the object, so the test grabs from two very
// different hand poses and expects the same outcome.
static int case_grab_socket() {
    // A weapon whose grip sits 12 cm back and 3 cm down from the origin,
    // rotated a quarter turn about Y -- i.e. nothing like identity.
    ptv::Transform socket;
    socket.position = ptv::Vec3(0.0f, -0.03f, 0.12f);
    socket.rotation =
        glm::angleAxis(glm::radians(90.0f), ptv::Vec3(0.0f, 1.0f, 0.0f));

    // Same solve as GrabMechanic::grip_hold_from_socket.
    const ptv::Quat inv_socket = glm::inverse(socket.rotation);
    const ptv::Vec3 local_offset = -(inv_socket * socket.position);
    const ptv::Quat local_rotation = inv_socket;

    struct HandPose {
        ptv::Vec3 position;
        ptv::Quat rotation;
    };
    const HandPose poses[2] = {
        {ptv::Vec3(0.2f, 1.1f, -0.4f),
         glm::angleAxis(glm::radians(35.0f), ptv::Vec3(0.0f, 1.0f, 0.0f))},
        // Reaching from a completely different angle.
        {ptv::Vec3(-0.7f, 0.6f, 0.9f),
         glm::angleAxis(glm::radians(-110.0f),
                        glm::normalize(ptv::Vec3(0.3f, 1.0f, 0.2f)))},
    };

    for (const auto& hand : poses) {
        // What the mechanic's per-frame kinematic follow will command.
        const ptv::Vec3 obj_pos =
            hand.position + hand.rotation * local_offset;
        const ptv::Quat obj_rot = hand.rotation * local_rotation;

        // Where the grip socket actually ends up.
        const ptv::Vec3 socket_pos = obj_pos + obj_rot * socket.position;
        const ptv::Quat socket_rot = obj_rot * socket.rotation;

        if (glm::length(socket_pos - hand.position) > 1e-4f) {
            std::fprintf(stderr,
                         "[fail] grip socket landed %.5f m from the hand\n",
                         glm::length(socket_pos - hand.position));
            return 1;
        }
        if (std::fabs(glm::dot(socket_rot, hand.rotation)) < 0.9999f)
            return fail("grip socket orientation does not match the hand");
    }

    return ok("grab socket: the grip lands in the hand from any approach");
}

// The clip contract: a mechanic asks for the semantic intent "walk" and the
// engine finds whatever the retargeter actually named that clip. Without this
// indirection every generated play_clip call is a coin flip, because Tripo
// picks the names and codegen never sees the asset.
static int case_clip_contract() {
    const char* doc = R"({
        "version": 1, "prompt_id": "t", "scene_id": "s", "title": "T", "name": "S",
        "player": {"height_meters": 1.7, "locomotion": "teleport", "comfort": "comfort",
                    "spawn": {"position": [0,0,0], "rotation": [0,0,0,1]}},
        "sky_color": [0,0,0], "ambient_intensity": 0.1,
        "lights": [],
        "entities": [
          {"id": "goblin", "name": "Goblin",
           "transform": {"position": [0,0,-2], "rotation": [0,0,0,1], "scale": 1.0},
           "mesh": {"asset_id": "goblin", "glb_path": "missing.glb",
                    "target_scale_meters": 1.6, "collision_shape": "capsule",
                    "skinned": true, "joint_count": 65,
                    "animations": {"idle": "Idle", "walk": "Walk_01",
                                   "run": "Run_Fast"}},
           "tags": ["enemy"]}
        ]
    })";
    const char* path = "ptv_smoke_clip_scene.json";
    std::FILE* f = std::fopen(path, "wb");
    std::fwrite(doc, 1, std::strlen(doc), f);
    std::fclose(f);

    auto loaded = ptv::LoadSceneJson(path);
    if (loaded.entities.size() != 1 || !loaded.entities[0].mesh.has_value())
        return fail("clip scene did not load");

    const auto& mesh = *loaded.entities[0].mesh;
    if (mesh.animations.size() != 3)
        return fail("animations map did not survive the scene parse");
    if (mesh.animations.at("walk") != "Walk_01")
        return fail("intent 'walk' resolved to the wrong clip name");

    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);
    const auto e = world.find("goblin");
    if (e == entt::null || !world.registry().all_of<ptv::CMesh>(e))
        return fail("goblin entity missing CMesh");

    const auto& cm = world.registry().get<ptv::CMesh>(e);
    if (cm.animations.size() != 3)
        return fail("animations map did not reach CMesh");
    // The resolution play_clip performs: intent in, real clip name out.
    if (cm.animations.at("run") != "Run_Fast")
        return fail("intent 'run' did not reach the ECS intact");
    // An intent the asset does not have must simply be absent, so play_clip
    // returns false instead of starting the wrong animation.
    if (cm.animations.count("shoot") != 0)
        return fail("an unrequested intent appeared in the map");

    return ok("clip contract: intents resolve to real clip names through the "
              "scene into the ECS");
}

static int case_openxr() {
    XrApplicationInfo ai{};
    std::strncpy(ai.applicationName, "ptv_smoke",
                 sizeof(ai.applicationName) - 1);
    ai.applicationVersion = 1;
    std::strncpy(ai.engineName, "ptv", sizeof(ai.engineName) - 1);
    ai.engineVersion = 1;
    ai.apiVersion = XR_CURRENT_API_VERSION;
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    ici.applicationInfo = ai;
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&ici, &inst);
    if (XR_FAILED(r)) {
        std::printf("[info] no OpenXR runtime present (ok in headless CI)\n");
        return 0;
    }
    xrDestroyInstance(inst);
    return ok("openxr: xrCreateInstance succeeded");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(
            stderr,
            "usage: ptv_smoke <mechanics|scene|openxr|skinning <glb>>\n");
        return 2;
    }
    std::string what = argv[1];
    if (what == "mechanics") return case_mechanics();
    if (what == "scene") return case_scene();
    if (what == "openxr") return case_openxr();
    if (what == "skinning") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke skinning <rigged.glb>\n");
            return 2;
        }
        return case_skinning(argv[2]);
    }
    if (what == "gltf_nodes") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke gltf_nodes <static.glb>\n");
            return 2;
        }
        return case_gltf_nodes(argv[2]);
    }
    if (what == "skinning_math") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: ptv_smoke skinning_math <rigged.glb>\n");
            return 2;
        }
        return case_skinning_math(argv[2]);
    }
    if (what == "skinning_gpu") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: ptv_smoke skinning_gpu <rigged.glb>\n");
            return 2;
        }
        return case_skinning_gpu(argv[2]);
    }
    if (what == "pose_system") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: ptv_smoke pose_system <rigged.glb>\n");
            return 2;
        }
        return case_pose_system(argv[2]);
    }
    if (what == "hand_asset") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke hand_asset <hand.glb>\n");
            return 2;
        }
        return case_hand_asset(argv[2]);
    }
    if (what == "hand_pose") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke hand_pose <hand.glb>\n");
            return 2;
        }
        return case_hand_pose(argv[2]);
    }
    if (what == "grab_socket") return case_grab_socket();
    if (what == "clip_contract") return case_clip_contract();
    if (what == "lod_skinning") {
        if (argc < 4) {
            std::fprintf(stderr,
                         "usage: ptv_smoke lod_skinning <lod0.glb> "
                         "<lod1.glb> [...]\n");
            return 2;
        }
        return case_lod_skinning(argc - 2, argv + 2);
    }
    std::fprintf(stderr, "unknown case: %s\n", argv[1]);
    return 2;
}
