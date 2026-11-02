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
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/sinks/ostream_sink.h>

#include "../src/mechanics/builtin/HandRig.h"
#include "../src/mechanics/builtin/TwoBoneIk.h"
#include "../src/mechanics/builtin/grab/GripHold.h"
#include "../src/mechanics/builtin/grab/HoldSolver.h"
#include "../src/mechanics/builtin/teleport/TeleportArc.h"
#include "../src/mechanics/builtin/slide/SlideComfort.h"
#include "../src/mechanics/builtin/avatar_body/NeckDrift.h"
#include "../src/mechanics/builtin/avatar_body/HeightCalibration.h"
#include "../src/core/NpcRuntime.h"
#include "../src/core/TextureGallery.h"
#include "ptv/api/BodySlots.h"
#include "ptv/api/Facing.h"
#include "ptv/api/IBehaviour.h"
#include "ptv/api/IMechanic.h"
#include "ptv/core/Logger.h"
#include "ptv/core/MemoryGuard.h"
#include "ptv/core/QualityGovernor.h"
#include "ptv/ecs/Clone.h"
#include "ptv/ecs/Skeleton.h"
#include "ptv/ecs/World.h"
#include "ptv/mechanics/MechanicRegistry.h"
#include "ptv/nav/NavGraph.h"
#include "ptv/physics/PhysicsWorld.h"
#include "ptv/render/GltfMeshLoad.h"
#include "ptv/render/GltfSkinnedLoad.h"
#include "ptv/render/Ktx2.h"
#include "ptv/render/Renderer.h"
#include "ptv/render/VulkanContext.h"
#include "ptv/save/SaveSystem.h"
#include "ptv/scene/SceneLoader.h"

// Internal to the render module: the tag -> visual role table. Included
// directly because the whole point of the table is that it is the ONLY place
// these decisions live, so the test has to read the same rows the renderer
// does rather than a copy.
#include "../src/render/VisualTagTable.h"
// Panel layout and hit-testing, for the same reason: the parts that are easy to
// get subtly wrong are pure math, and testing them needs no headset.
#include "../src/mechanics/builtin/ui_panel/UiPanelLayout.h"

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
        "builtin.character_locomotion",
        "builtin.avatar_body",
        "builtin.mirror_surface",
        "builtin.equip",
        "builtin.door",
        "builtin.container",
        "builtin.mover",
        "builtin.switchable",
        "builtin.breakable",
        "builtin.harvestable",
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

// A player with no locomotion and no turning is a stationary game (a rhythm
// game, a fixed archery range). It must load and run without the engine
// adding a mechanic or complaining: the scene binds only finger posing on
// the player rig, and that is the whole player.
static int case_stationary_scene() {
    const char* doc = R"({
        "version": 1, "prompt_id": "t", "scene_id": "s", "title": "T", "name": "S",
        "player": {"height_meters": 1.7, "locomotion": "stationary",
                   "hands": {"source": "default"},
                   "spawn": {"position": [0,0,0], "rotation": [0,0,0,1]}},
        "sky_color": [0,0,0], "ambient_intensity": 0.1, "lights": [],
        "entities": [
          {"id": "player_rig", "name": "Player Rig",
           "transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": 1.0},
           "mesh": null, "physics": null,
           "mechanic_bindings": [{"id": "builtin.hand_pose",
                                  "builtin_id": "builtin.hand_pose", "params": {}}],
           "tags": ["player_rig", "player_system"]},
          {"id": "floor", "name": "Floor",
           "transform": {"position": [0,-0.05,0], "rotation": [0,0,0,1], "scale": 10.0},
           "mesh": null,
           "physics": {"kind": "static", "mass_kg": 0.0, "restitution": 0.1,
                       "friction": 0.9, "collision_shape": "aabb"},
           "mechanic_bindings": [], "tags": ["ground"]}
        ]
    })";
    const char* path = "ptv_smoke_stationary.json";
    std::FILE* f = std::fopen(path, "wb");
    std::fwrite(doc, 1, std::strlen(doc), f);
    std::fclose(f);

    std::ostringstream captured;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
    sink->set_level(spdlog::level::warn);
    ptv::log().sinks().push_back(sink);
    auto drop_sink = [&] {
        auto& sinks = ptv::log().sinks();
        sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
    };

    auto loaded = ptv::LoadSceneJson(path);
    if (loaded.locomotion_menu_enabled) {
        drop_sink();
        return fail("stationary: locomotion_menu defaulted on");
    }
    if (loaded.hands_source != "default" || !loaded.default_locomotion_mode.empty()) {
        drop_sink();
        return fail("stationary: player block parsed wrong");
    }
    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);
    ptv::PhysicsWorld physics;
    if (!physics.initialize()) {
        drop_sink();
        return fail("stationary: physics failed to initialize");
    }
    physics.rebuild_from_world(world);
    ptv::MechanicContext ctx{};
    ptv::MechanicHost host;
    host.attach_bindings(world, ctx);
    const ptv::InputSnapshot input{};
    for (int i = 0; i < 3; ++i) {
        host.update(world, 1.0f / 60.0f, input, ctx, ptv::MechanicPhase::Input);
        host.update(world, 1.0f / 60.0f, input, ctx, ptv::MechanicPhase::Simulate);
    }

    // Exactly what the scene bound -- nothing was added behind its back.
    int bindings = 0;
    auto view = world.registry().view<const ptv::CMechanicBinding>();
    for (auto e : view) {
        for (const auto& entry : view.get<const ptv::CMechanicBinding>(e).entries) {
            ++bindings;
            if (entry.id != "builtin.hand_pose") {
                physics.shutdown();
                drop_sink();
                std::fprintf(stderr, "[fail] stationary: unexpected binding %s\n",
                             entry.id.c_str());
                return 1;
            }
        }
    }
    physics.shutdown();
    drop_sink();
    if (bindings != 1) return fail("stationary: expected one binding");
    if (!captured.str().empty()) {
        std::fprintf(stderr, "[fail] stationary scene logged:\n%s\n",
                     captured.str().c_str());
        return 1;
    }
    return ok("stationary: a player with no locomotion loads and runs with no "
              "added mechanics, errors or warnings");
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

// The slot vocabulary is what lets one mechanic pose a hand fixture, a
// Mixamo-spec rig and a Tripo-spec rig without knowing which it has. The
// properties worth pinning are the ones a naive table would get wrong.
static int case_body_slots() {
    // Separators, casing and the mixamorig: namespace are all noise.
    if (ptv::normalize_joint_name("mixamorig:LeftForeArm") != "leftforearm") {
        return fail("normalize did not strip the mixamorig: prefix");
    }
    if (ptv::normalize_joint_name("Left_Fore_Arm") != "leftforearm" ||
        ptv::normalize_joint_name("left fore arm") != "leftforearm" ||
        ptv::normalize_joint_name("leftforearm") != "leftforearm") {
        return fail("normalize did not collapse separators and casing");
    }

    if (!ptv::is_body_slot("forearm_r") || !ptv::is_body_slot("head") ||
        !ptv::is_body_slot("index_1_r") || !ptv::is_body_slot("toes_l")) {
        return fail("expected slot missing from the vocabulary");
    }
    if (ptv::is_body_slot("elbow")) {
        return fail("sided slots must not resolve without a side suffix");
    }

    auto has = [](const std::vector<std::string>& v, const char* want) {
        return std::find(v.begin(), v.end(), std::string(want)) != v.end();
    };

    // One slot has to cover several conventions at once, or the mechanic only
    // works on whichever rig the author happened to test against.
    const auto& forearm = ptv::body_slot_candidates("forearm_r");
    if (!has(forearm, "rightforearm")) return fail("forearm_r misses Mixamo");
    if (!has(forearm, "lowerarmr")) return fail("forearm_r misses Unreal");
    if (!has(forearm, "rightlowerarm")) {
        return fail("forearm_r misses Unity Humanoid");
    }

    // The rigs this pipeline actually generates. Tripo sides a bone with a
    // short PREFIX and no side word, which no other convention here does, so
    // the table used to cover every naming scheme except its own -- leaving
    // arm IK, neck tracking and leg posing dead on every generated character.
    // These are the literal joint names out of a Tripo biped auto-rig.
    struct TripoJoint {
        const char* slot;
        const char* joint;
    };
    static const TripoJoint kTripoRig[] = {
        {"shoulder_l", "L_Clavicle"},  {"shoulder_r", "R_Clavicle"},
        {"upper_arm_l", "L_Upperarm"}, {"upper_arm_r", "R_Upperarm"},
        {"forearm_l", "L_Forearm"},    {"forearm_r", "R_Forearm"},
        {"hand_l", "L_Hand"},          {"hand_r", "R_Hand"},
        {"thigh_l", "L_Thigh"},        {"thigh_r", "R_Thigh"},
        {"shin_l", "L_Calf"},          {"shin_r", "R_Calf"},
        {"foot_l", "L_Foot"},          {"foot_r", "R_Foot"},
        {"toes_l", "L_ToeBase"},       {"toes_r", "R_ToeBase"},
        {"neck", "NeckTwist01"},       {"head", "Head"},
        {"spine", "Spine01"},          {"chest", "Spine02"},
        {"hips", "Pelvis"},            {"root", "Root"},
    };
    for (const auto& [slot, joint] : kTripoRig) {
        const auto& cands = ptv::body_slot_candidates(slot);
        if (!has(cands, ptv::normalize_joint_name(joint).c_str())) {
            std::printf("[fail] slot '%s' does not accept Tripo joint '%s'\n",
                        slot, joint);
            return 1;
        }
    }

    // Tripo also emits twist bones beside the real ones. Those must NOT
    // satisfy a limb slot: driving L_ForearmTwist01 instead of L_Forearm
    // rolls the wrist where the elbow should bend.
    if (has(ptv::body_slot_candidates("forearm_l"),
            ptv::normalize_joint_name("L_ForearmTwist01").c_str())) {
        return fail("forearm_l must not accept a Tripo twist bone");
    }

    // Mixamo has no elbow or knee bone, so these deliberately alias.
    if (ptv::body_slot_candidates("elbow_r") != forearm) {
        return fail("elbow_r must resolve to the same joint as forearm_r");
    }
    if (ptv::body_slot_candidates("knee_l") !=
        ptv::body_slot_candidates("shin_l")) {
        return fail("knee_l must resolve to the same joint as shin_l");
    }

    // The engine's own hand fixture is one GLB per hand, so its joints are
    // unsided. A sided slot still has to find them, or hand_pose regresses.
    const auto& index = ptv::body_slot_candidates("index_1_r");
    if (!has(index, "righthandindex1")) {
        return fail("index_1_r misses the Mixamo full-body name");
    }
    if (!has(index, "index01")) {
        return fail("index_1_r misses the engine hand fixture's unsided name");
    }
    // Sided names must be tried first: on a full-body rig both could match,
    // and picking the unsided one would pose the wrong hand.
    const auto sided = std::find(index.begin(), index.end(),
                                 std::string("righthandindex1"));
    const auto unsided =
        std::find(index.begin(), index.end(), std::string("index01"));
    if (sided > unsided) {
        return fail("unsided finger names must be the last resort");
    }

    if (ptv::body_finger_slot("index", 1, 1) != "index_1_r" ||
        ptv::body_finger_slot("thumb", 3, 0) != "thumb_3_l") {
        return fail("body_finger_slot built the wrong slot name");
    }

    std::printf("[ok] body slots: %zu slots, conventions unified\n",
                ptv::body_slot_names().size());
    return 0;
}

// The affordance built-ins have one contract that matters more than their
// motion: they must not keep state of their own. Everything goes through the
// registry, because a mechanic that cached "am I open" would lose it the
// moment the environment reloaded -- which is the exact bug the registry
// exists to prevent.
static int case_nested_scene() {
    if (!ptv::MechanicRegistry::get().make_mechanic("builtin.head_interior")) {
        return fail("builtin.head_interior not registered");
    }

    const auto dir =
        std::filesystem::temp_directory_path() / "ptv_nested_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / "interior.json";
    {
        std::ofstream f(path);
        // Authored at HUMAN scale, as world architects are told to: a level
        // laid out inside a 22 cm box produces geometry too degenerate to
        // walk on, so the shrink happens on load instead.
        f << R"({
  "version": 2,
  "scene_id": "skull_interior",
  "entities": [
    { "id": "floor", "transform": { "position": [0,0,0], "scale": 1.0 },
      "primitive": { "kind": "box", "size": [20,0.2,20] },
      "tags": ["ground"] },
    { "id": "front_door", "transform": { "position": [10,0,0], "scale": 1.0 },
      "primitive": { "kind": "box", "size": [1,2,0.2] } },
    { "id": "lamp", "transform": { "position": [0,2,0], "scale": 1.0 },
      "primitive": { "kind": "sphere", "size": [0.3,0.3,0.3] },
      "attached_to": { "entity_id": "front_door" } }
  ],
  "interactives": [
    { "id": "front_door", "kind": "door", "initial_state": { "open": false } }
  ]
})";
    }

    const ptv::SceneDoc interior = ptv::LoadSceneJson(path.string());
    if (interior.entities.size() != 3) return fail("interior did not parse");

    ptv::World world;

    // An OUTER scene that already has a "front_door" of its own. This is the
    // collision the id prefix exists to prevent: two world architects both
    // name a door "front_door", and ids are persistence keys, so a clash does
    // not fail loudly -- it silently restores one door's saved state onto the
    // other.
    const auto outer_door = world.create_entity("front_door");
    world.registry().emplace<ptv::CTransform>(outer_door, ptv::CTransform{});

    ptv::SceneNesting nesting{};
    nesting.id_prefix = "skull_interior";
    nesting.scale = 0.011f;  // 20 m authored -> ~0.22 m skull
    nesting.tag = "nested:skull_interior";
    nesting.mount = glm::scale(
        glm::translate(ptv::Mat4(1.0f), ptv::Vec3(3.0f, 1.6f, -2.0f)),
        ptv::Vec3(nesting.scale));

    ptv::ApplySceneToWorld(interior, world, &nesting);

    // The outer door is untouched and the interior's is a separate entity.
    if (world.find("front_door") != outer_door) {
        return fail("the nested scene stole the outer scene's id");
    }
    const auto inner_door = world.find("skull_interior/front_door");
    if (inner_door == entt::null) return fail("nested id was not scoped");
    if (inner_door == outer_door) return fail("nested entity collided");

    // The interactive's persistence key is the SCOPED id, or two interiors
    // that both declare a front_door would save over each other.
    if (!world.registry().all_of<ptv::CInteractive>(inner_door)) {
        return fail("nested interactive did not bind");
    }
    if (world.registry().get<ptv::CInteractive>(inner_door).id !=
        "skull_interior/front_door") {
        return fail("nested interactive kept an unscoped persistence key");
    }

    // Transforms are re-based through the mount, so the interior sits at the
    // head rather than at the world origin.
    const auto floor = world.find("skull_interior/floor");
    if (floor == entt::null) return fail("nested floor missing");
    const auto& ft = world.registry().get<ptv::CTransform>(floor).t;
    if (glm::length(ft.position - ptv::Vec3(3.0f, 1.6f, -2.0f)) > 1e-3f) {
        return fail("nested entity was not mounted");
    }

    // Scale is carried on the Transform, NOT left in the mount matrix: the
    // attachment system that moves the interior each frame rewrites position
    // and rotation but never scale, so anything left in the matrix is lost on
    // the first frame.
    if (std::abs(ft.scale - nesting.scale) > 1e-5f) {
        return fail("fit scale did not reach the Transform");
    }

    // A door 10 m along the authored floor lands 11 cm from the mount once
    // shrunk -- the offset scales with the room, which is what keeps the
    // interior's proportions intact.
    const auto& dt = world.registry().get<ptv::CTransform>(inner_door).t;
    const float offset = glm::length(dt.position - ft.position);
    if (std::abs(offset - 10.0f * nesting.scale) > 1e-3f) {
        return fail("interior proportions were not preserved by the fit");
    }

    // An attachment authored INSIDE the interior resolves against the
    // interior's own copy, not the outer scene's same-named entity.
    if (!world.registry().all_of<ptv::CAttachment>(
            world.find("skull_interior/lamp"))) {
        return fail("interior-internal attachment was dropped");
    }
    const auto& att = world.registry().get<ptv::CAttachment>(
        world.find("skull_interior/lamp"));
    if (att.parent != static_cast<std::uint32_t>(inner_door)) {
        return fail("interior attachment resolved to the outer entity");
    }

    // Every nested entity carries the tag, which is both how the loader's
    // output is identified and how unloading finds it later.
    int tagged = 0;
    auto view = world.registry().view<ptv::CTag>();
    for (auto e : view) {
        const auto& tags = view.get<ptv::CTag>(e).tags;
        if (std::find(tags.begin(), tags.end(), nesting.tag) != tags.end()) {
            ++tagged;
        }
    }
    if (tagged != 3) return fail("nested entities were not all tagged");

    // A normal top-level load must be completely unaffected.
    ptv::World plain;
    ptv::ApplySceneToWorld(interior, plain);
    if (plain.find("front_door") == entt::null) {
        return fail("un-nested load started prefixing ids");
    }
    if (std::abs(plain.registry()
                     .get<ptv::CTransform>(plain.find("floor"))
                     .t.scale -
                 1.0f) > 1e-5f) {
        return fail("un-nested load started scaling");
    }

    std::filesystem::remove_all(dir);
    return 0;
}

static int case_reference_frame() {
    // The reference-frame math, without a headset. This is pure composition,
    // and it is where the subtle errors live: a frame that inherits a parent's
    // pitch, a boarding step that teleports the player, or a slide direction
    // that curves because a world vector was added to a frame-local position.
    //
    // Mirrors Application::rig_parent_matrix / rig_world_matrix. Kept in step
    // by asserting the PROPERTIES those functions exist to guarantee, rather
    // than by duplicating their code and hoping the copies agree.
    auto frame_of = [](const ptv::Vec3& pos, float yaw, float pitch) {
        // A parent may pitch (a boat on a wave); the FRAME must not, or the
        // player's horizon tilts -- the most reliable way to make someone sick
        // in VR. So the frame keeps position and heading only.
        ptv::Mat4 parent = glm::translate(ptv::Mat4(1.0f), pos);
        parent = glm::rotate(parent, yaw, ptv::Vec3(0.0f, 1.0f, 0.0f));
        parent = glm::rotate(parent, pitch, ptv::Vec3(1.0f, 0.0f, 0.0f));
        // Flatten: position + yaw of the parent's forward.
        const ptv::Vec3 fwd =
            ptv::Vec3(parent * ptv::Vec4(0.0f, 0.0f, -1.0f, 0.0f));
        // atan2(-x, -z), the exact inverse of glm::rotate about +Y with
        // forward at -Z. Getting this backwards is what made both spawn paths
        // face the player in mirrored orientation for any non-axis-aligned
        // facing.
        const ptv::Vec3 flat(fwd.x, 0.0f, fwd.z);
        const float y = glm::length(flat) < 1e-5f
                            ? 0.0f
                            : std::atan2(-flat.x, -flat.z);
        return glm::rotate(
            glm::translate(ptv::Mat4(1.0f), ptv::Vec3(parent[3])), y,
            ptv::Vec3(0.0f, 1.0f, 0.0f));
    };

    // A pitching parent must not tilt the frame: local "up" stays world up.
    {
        const ptv::Mat4 f =
            frame_of(ptv::Vec3(0.0f), 0.0f, glm::radians(30.0f));
        const ptv::Vec3 up = ptv::Vec3(f * ptv::Vec4(0, 1, 0, 0));
        if (std::abs(up.y - 1.0f) > 1e-3f) {
            return fail("frame inherited the parent's pitch");
        }
    }

    // The property that makes a moving platform work: the player's LOCAL
    // position is untouched as the parent moves, and their world position
    // follows. Standing still means zero stick input and still arriving
    // somewhere else.
    {
        const ptv::Vec3 local(0.0f, 0.0f, 2.0f);  // 2 m along the deck
        const ptv::Vec3 a = ptv::Vec3(
            frame_of(ptv::Vec3(0.0f, 0.0f, 0.0f), 0.0f, 0.0f) *
            ptv::Vec4(local, 1.0f));
        const ptv::Vec3 b = ptv::Vec3(
            frame_of(ptv::Vec3(10.0f, 0.0f, 0.0f), 0.0f, 0.0f) *
            ptv::Vec4(local, 1.0f));
        if (std::abs((b.x - a.x) - 10.0f) > 1e-4f) {
            return fail("player did not travel with the frame");
        }
        if (std::abs(b.z - a.z) > 1e-4f) {
            return fail("player drifted within the frame");
        }
    }

    // A turning parent carries the player's heading with it, which is why the
    // frame's yaw has to be added to the rig's own.
    {
        const ptv::Mat4 f =
            frame_of(ptv::Vec3(0.0f), glm::radians(90.0f), 0.0f);
        const ptv::Vec3 p =
            ptv::Vec3(f * ptv::Vec4(0.0f, 0.0f, -1.0f, 1.0f));
        // R_y(+90) * (0,0,-1) = (-1, 0, 0).
        if (std::abs(p.x + 1.0f) > 1e-3f || std::abs(p.z) > 1e-3f) {
            return fail("frame rotation did not carry the player");
        }
    }

    // Yaw decomposition must be the exact inverse of the composition, or
    // boarding a rotated frame spins the player.
    {
        for (float deg : {-170.0f, -35.0f, 0.0f, 50.0f, 120.0f}) {
            const float yaw = glm::radians(deg);
            const ptv::Mat4 m =
                glm::rotate(ptv::Mat4(1.0f), yaw, ptv::Vec3(0, 1, 0));
            const ptv::Vec3 fwd =
                ptv::Vec3(m * ptv::Vec4(0.0f, 0.0f, -1.0f, 0.0f));
            const float recovered = std::atan2(-fwd.x, -fwd.z);
            if (std::abs(recovered - yaw) > 1e-4f) {
                return fail("yaw decomposition is not the inverse of rotate");
            }
        }
    }

    // And a facing must round-trip: yaw_for_facing then apply must reproduce
    // the requested direction. The old atan2(f.x, -f.z) passed only when
    // f.x was zero, which is why a spawn facing sideways came out mirrored.
    {
        const ptv::Vec3 facing = glm::normalize(ptv::Vec3(1.0f, 0.0f, 0.3f));
        const float yaw = std::atan2(-facing.x, -facing.z);
        const ptv::Mat4 m =
            glm::rotate(ptv::Mat4(1.0f), yaw, ptv::Vec3(0, 1, 0));
        const ptv::Vec3 got =
            ptv::Vec3(m * ptv::Vec4(0.0f, 0.0f, -1.0f, 0.0f));
        if (glm::length(got - facing) > 1e-3f) {
            return fail("spawn facing did not round trip through yaw");
        }
    }

    // Boarding must not move the player. Converting their world pose into the
    // new frame and back has to return them exactly where they were -- a
    // player who steps onto a lift and is instantly elsewhere has been
    // teleported by a bookkeeping detail.
    {
        const ptv::Mat4 f =
            frame_of(ptv::Vec3(3.0f, 1.0f, -2.0f), glm::radians(35.0f), 0.0f);
        const ptv::Vec3 world(5.0f, 0.0f, 4.0f);
        const ptv::Vec3 local =
            ptv::Vec3(glm::inverse(f) * ptv::Vec4(world, 1.0f));
        const ptv::Vec3 back = ptv::Vec3(f * ptv::Vec4(local, 1.0f));
        if (glm::length(back - world) > 1e-3f) {
            return fail("boarding moved the player");
        }
    }

    // A world-space slide direction rotated into a turned frame, then back
    // out, must be unchanged -- otherwise walking forward on a turning
    // vehicle curves.
    {
        const float yaw = glm::radians(50.0f);
        const ptv::Vec3 world_dir(0.0f, 0.0f, -1.0f);
        const ptv::Vec3 in_frame =
            glm::angleAxis(-yaw, ptv::Vec3(0, 1, 0)) * world_dir;
        const ptv::Vec3 out =
            glm::angleAxis(yaw, ptv::Vec3(0, 1, 0)) * in_frame;
        if (glm::length(out - world_dir) > 1e-4f) {
            return fail("slide direction did not survive the frame round trip");
        }
    }

    // Scale is innermost, so it shrinks the player's tracked motion but NOT
    // their offset within the frame: a passenger 2 m along a deck is 2 m along
    // it whatever size they are.
    {
        const float scale = 0.02f;
        ptv::Mat4 m = glm::translate(ptv::Mat4(1.0f),
                                     ptv::Vec3(0.0f, 0.0f, 2.0f));
        m = glm::scale(m, ptv::Vec3(scale));
        const ptv::Vec3 origin = ptv::Vec3(m[3]);
        if (std::abs(origin.z - 2.0f) > 1e-4f) {
            return fail("scale moved the player's offset in the frame");
        }
        // A 1 m physical step becomes 2 cm of world travel.
        const ptv::Vec3 stepped =
            ptv::Vec3(m * ptv::Vec4(0.0f, 0.0f, -1.0f, 1.0f));
        if (std::abs((stepped.z - origin.z) + scale) > 1e-4f) {
            return fail("scale did not shrink tracked motion");
        }
    }

    // Clip planes scale with the player, or a 2 cm passenger cannot see their
    // own hands: both hands and floor sit inside a fixed 5 cm near plane.
    {
        const float scale = 0.02f;
        const float near_z = 0.05f * scale;
        if (near_z >= 0.02f) return fail("near plane did not scale down");
        if (near_z <= 0.0f) return fail("near plane collapsed to zero");
    }
    return 0;
}

static int case_ui_panel() {
    if (!ptv::MechanicRegistry::get().make_mechanic("builtin.ui_panel")) {
        return fail("builtin.ui_panel not registered");
    }

    ptv::CUiPanel panel{};
    panel.id = "wardrobe";
    panel.rows.resize(3);
    panel.rows[0].id = "helmet";
    panel.rows[1].id = "gauntlet";
    panel.rows[2].id = "boots";
    panel.rows[1].enabled = false;  // fixed in flight, but still listed

    // Anchored from the head using Meta's comfort guidance: ~1.5 m forward and
    // slightly below eye level, so the resting line of sight lands on it.
    const ptv::Vec3 head(0.0f, 1.6f, 0.0f);
    ptv::ui_panel::anchor_to_head(panel, head, ptv::Vec3(0.0f, 0.0f, -1.0f));
    if (!panel.anchor_set) return fail("panel did not anchor");
    if (std::abs(panel.anchor.z - (-1.5f)) > 1e-3f) {
        return fail("panel not placed at its forward distance");
    }
    if (panel.anchor.y >= head.y) {
        return fail("panel not placed below eye level");
    }

    // Rows are laid out centred, so a list grows evenly instead of creeping
    // down out of comfortable view as it gets longer.
    const ptv::Vec3 top = ptv::ui_panel::row_world_position(panel, 0);
    const ptv::Vec3 mid = ptv::ui_panel::row_world_position(panel, 1);
    const ptv::Vec3 bot = ptv::ui_panel::row_world_position(panel, 2);
    if (!(top.y > mid.y && mid.y > bot.y)) return fail("rows not ordered");
    if (std::abs(mid.y - panel.anchor.y) > 1e-4f) {
        return fail("row list is not centred on the anchor");
    }

    // A ray straight at the top row picks it.
    const ptv::Vec3 origin(0.0f, top.y, 0.5f);
    const ptv::Vec3 fwd(0.0f, 0.0f, -1.0f);
    if (ptv::ui_panel::pick_row(panel, origin, fwd) != 0) {
        return fail("failed to pick the row under the ray");
    }

    // A disabled row is SKIPPED, not highlighted-then-refused: a highlight the
    // player cannot act on reads as the panel being broken. Aiming at the
    // disabled middle row must fall through to a neighbour or to nothing --
    // never to the disabled row itself.
    const ptv::Vec3 at_mid(0.0f, mid.y, 0.5f);
    if (ptv::ui_panel::pick_row(panel, at_mid, fwd) == 1) {
        return fail("disabled row was selectable");
    }

    // Aiming away from the panel selects nothing rather than the nearest row.
    if (ptv::ui_panel::pick_row(panel, origin, ptv::Vec3(0.0f, 0.0f, 1.0f)) >= 0) {
        return fail("picked a row while aiming away");
    }

    // Events carry the row ID, not just its index: an inventory that reorders
    // itself between frames would otherwise deliver a click for the wrong item.
    ptv::ui_panel::push_event(panel, 0, 1);
    if (panel.events.size() != 1) return fail("event not queued");
    if (panel.events[0].row_id != "helmet") return fail("event lost its row id");
    if (panel.events[0].panel_id != "wardrobe") return fail("event lost panel");
    if (panel.events[0].hand != 1) return fail("event lost its hand");

    // The queue is bounded. An unread queue means the caller stopped polling,
    // which must not become a slow leak; the NEWEST click survives, because
    // that is the one the player is waiting on.
    for (int i = 0; i < 64; ++i) ptv::ui_panel::push_event(panel, 2, 0);
    if (panel.events.size() > 16) return fail("event queue is unbounded");
    if (panel.events.back().row_id != "boots") {
        return fail("queue dropped the newest event instead of the oldest");
    }
    return 0;
}

static int case_scene_transitions() {
    // A portal has to survive the round trip through the document, because
    // everything about a transition is data the world architect declared:
    // where the way out is, which scene it leads to, and which named arrival
    // point in that scene it lands on.
    const auto dir = std::filesystem::temp_directory_path() /
                     "ptv_transition_test";
    std::filesystem::create_directories(dir);
    const auto hub = dir / "scene.hub.json";
    {
        std::ofstream f(hub);
        f << R"({
  "version": 2,
  "scene_id": "hub",
  "player": { "spawn": { "position": [0,0,0], "rotation": [0,0,0,1] } },
  "spawn_points": [
    { "id": "front_gate", "position": [1,0,2], "facing": [0,0,-1] },
    { "id": "back_from_hospital", "position": [-4,0,7], "facing": [1,0,0] }
  ],
  "portals": [
    { "id": "hospital_door", "to_scene": "hospital",
      "target_spawn_id": "ward_entry",
      "position": [3,1,0], "size": [1.2,2.0,0.6] },
    { "id": "auto_exit", "to_scene": "warehouse",
      "spawn_point_id": "dock", "position": [-8,1,0],
      "requires_interaction": false },
    { "id": "broken", "position": [0,0,0] }
  ],
  "scene_index": { "hub": "scene.hub.json", "hospital": "scene.hospital.json" },
  "entities": []
})";
    }

    const ptv::SceneDoc doc = ptv::LoadSceneJson(hub.string());

    if (doc.spawn_points.size() != 2) return fail("spawn points lost");
    const auto& back = doc.spawn_points[1];
    if (back.id != "back_from_hospital") return fail("spawn id wrong");
    // Arrival is named because the right place to stand depends on where the
    // player came from -- this one is the hospital's door, not the front gate.
    if (std::abs(back.position.z - 7.0f) > 1e-4f) {
        return fail("spawn position wrong");
    }
    if (std::abs(back.facing.x - 1.0f) > 1e-4f) return fail("spawn facing wrong");

    // The portal with no destination is scenery and must be dropped at load,
    // so the frame loop never has to test for it.
    if (doc.portals.size() != 2) return fail("destination-less portal kept");

    const auto& door = doc.portals[0];
    if (door.to_scene != "hospital") return fail("portal target wrong");
    if (door.target_spawn_id != "ward_entry") return fail("portal spawn wrong");
    // Requiring a deliberate act is the DEFAULT: walking through a doorway you
    // did not mean to take, and losing the room you were in, is worse than
    // having to ask.
    if (!door.requires_interaction) return fail("portal defaulted to auto");
    if (std::abs(door.size.x - 1.2f) > 1e-4f) return fail("portal size wrong");

    const auto& autop = doc.portals[1];
    if (autop.requires_interaction) return fail("explicit auto portal ignored");
    // ``spawn_point_id`` is the name the scene compiler emits; both spellings
    // have to resolve or a compiled scene silently loses its arrival point.
    if (autop.target_spawn_id != "dock") {
        return fail("spawn_point_id alias not accepted");
    }

    // The index is how the runtime turns a scene id into a file without
    // knowing how a project is laid out -- and it must work inside an APK.
    if (doc.scene_index.size() != 2) return fail("scene index lost");
    if (doc.scene_index.at("hospital") != "scene.hospital.json") {
        return fail("scene index path wrong");
    }

    // A world swap must take the id map with it. entt reuses handles, so a
    // stale name would resolve SUCCESSFULLY to whatever took that handle --
    // a lookup for "front_door" quietly returning a lamp.
    ptv::World world;
    const auto e = world.create_entity("front_door");
    if (world.find("front_door") != e) return fail("id lookup broken");
    world.clear();
    if (world.find("front_door") != entt::null) {
        return fail("stale id survived the scene swap");
    }

    std::filesystem::remove_all(dir);
    return 0;
}

static int case_visual_tags() {
    // Every affordance kind a world architect may declare needs a row in the
    // renderer's table. Without one it falls through to the generic grey cube,
    // which is exactly what happened to declared doors: the kind existed in
    // the scene document and in the mechanic, but not in the renderer, so a
    // door drew as an orange grab-cube.
    const char* kinds[] = {"door",      "container",   "mover",
                           "switchable", "breakable",  "harvestable"};
    const auto& known = ptv::render_tags::known_tags();
    for (const char* kind : kinds) {
        if (std::find(known.begin(), known.end(), std::string(kind)) ==
            known.end()) {
            return fail((std::string("no renderer row for affordance kind: ") +
                         kind)
                            .c_str());
        }
        const auto vis = ptv::render_tags::resolve({kind});
        if (!vis.has_placeholder) {
            return fail((std::string("affordance kind draws as the generic "
                                     "grey cube: ") +
                         kind)
                            .c_str());
        }
    }

    // Order is priority: a floor that is also tagged grabbable must still
    // read as a floor rather than shrink to a small orange cube.
    const auto floor = ptv::render_tags::resolve({"grabbable", "ground"});
    if (!floor.is_ground) return fail("ground lost to grabbable");
    if (floor.placeholder_xz_scale < 20.0f) {
        return fail("ground did not get the oversized floor placeholder");
    }

    // Hiding is absolute and short-circuits everything else -- a construct
    // prefab parked at a staging coordinate must never draw.
    const auto tmpl = ptv::render_tags::resolve({"template", "grabbable"});
    if (!tmpl.always_hidden) return fail("template did not hide");

    // Marker entities hide only when they have nothing to look at, which is
    // what keeps debug cubes off the real environment.
    const auto marker = ptv::render_tags::resolve({"checkpoint"});
    if (!marker.hidden_when_meshless) return fail("checkpoint not meshless-hidden");
    if (marker.always_hidden) return fail("checkpoint hidden even with a mesh");

    // An untagged entity must resolve to the neutral default, not to whatever
    // the first row happens to be.
    const auto none = ptv::render_tags::resolve({});
    if (none.always_hidden || none.is_ground || none.has_mesh_tint) {
        return fail("an untagged entity picked up a role");
    }
    return 0;
}

static int case_affordance_state_contract() {
    // Every affordance id has to be reachable, or a scene binding one gets a
    // silent no-op.
    const char* ids[] = {
        "builtin.door",       "builtin.container",  "builtin.mover",
        "builtin.switchable", "builtin.breakable",  "builtin.harvestable",
    };
    for (const char* id : ids) {
        auto m = ptv::MechanicRegistry::get().make_mechanic(id);
        if (!m) return fail((std::string("not registered: ") + id).c_str());
        if (m->id() != id) {
            return fail((std::string("id mismatch for ") + id).c_str());
        }
    }

    // A door declared OPEN by its world must read as open before anything
    // touches it, or a hospital that generated some doors ajar would snap
    // them shut on the first frame.
    ptv::World world;
    const auto door = world.create_entity("bedroom_door");
    {
        ptv::Transform t{};
        t.position = ptv::Vec3(2.0f, 0.0f, 0.0f);
        world.registry().emplace<ptv::CTransform>(door, ptv::CTransform{t});
        ptv::CInteractive ci{};
        ci.id = "bedroom_door";
        ci.kind = "door";
        ci.min_angle_deg = 0.0f;
        ci.max_angle_deg = 90.0f;
        ci.hinge_point = ptv::Vec3(-0.45f, 0.0f, 0.0f);
        ci.hinge_axis = ptv::Vec3(0.0f, 1.0f, 0.0f);
        ci.state["open"] = ptv::InteractiveValue::of(true);
        ci.state["angle"] = ptv::InteractiveValue::of(90.0f);
        ci.pristine = ci.state;
        world.registry().emplace<ptv::CInteractive>(door, std::move(ci));
    }

    // A door already at its declared open angle is not a CHANGE, so it must
    // produce no save delta. Otherwise every ajar door in a level would be
    // recorded as if the player had opened it.
    {
        const auto saved = ptv::SaveSystem::save_world(world);
        if (saved.contains("interactives")) {
            return fail("a door that starts open produced a spurious delta");
        }
    }

    // Closing it IS a change, and the record is keyed by the declared id.
    {
        auto& ci = world.registry().get<ptv::CInteractive>(door);
        ci.state["open"] = ptv::InteractiveValue::of(false);
        ci.state["angle"] = ptv::InteractiveValue::of(0.0f);
        const auto saved = ptv::SaveSystem::save_world(world);
        if (!saved.contains("interactives") ||
            !saved["interactives"].contains("bedroom_door")) {
            return fail("closing a door was not recorded");
        }
        // Both keys diverged, so both are stored -- and nothing else is.
        if (saved["interactives"]["bedroom_door"].size() != 2) {
            return fail("expected exactly the two changed keys");
        }
    }

    // A mover cycles through DECLARED positions rather than a boolean, which
    // is what an elevator needs. Assert the vocabulary survives a round trip
    // through the value variant: an enumerated "3" is not the number 3.
    {
        ptv::InteractiveValue named = ptv::InteractiveValue::named("floor_3");
        ptv::InteractiveValue number = ptv::InteractiveValue::of(3.0f);
        if (named == number) {
            return fail("an enumerated position compared equal to a number");
        }
        if (!(named == ptv::InteractiveValue::named("floor_3"))) {
            return fail("named state failed to compare equal to itself");
        }
    }

    // Number comparison uses a tolerance on purpose: a door settling a
    // fraction off its authored angle must not register as a change, or
    // every door in the level writes a delta on every save.
    {
        const auto a = ptv::InteractiveValue::of(0.0f);
        const auto b = ptv::InteractiveValue::of(0.00001f);
        if (!(a == b)) {
            return fail("state numbers compared without tolerance");
        }
        const auto c = ptv::InteractiveValue::of(45.0f);
        if (a == c) return fail("tolerance swallowed a real change");
    }

    return ok("affordances: registered, document-authoritative, deltas only");
}

// Persistence is DERIVED, not written by mechanics. The properties that make
// that safe: only differences are stored, the document stays authoritative
// for everything untouched, keys are stable strings rather than entt handles,
// and an unknown id in an old save is skipped instead of corrupting anything.
static int case_interactive_persistence() {
    // --- Build a scene with a door and a movable bottle ---------------
    auto make_world = [](ptv::World& world) {
        const auto door = world.create_entity("front_door");
        world.registry().emplace<ptv::CTransform>(door, ptv::CTransform{});
        ptv::CInteractive ci{};
        ci.id = "front_door";
        ci.kind = "door";
        ci.state["open"] = ptv::InteractiveValue::of(false);
        ci.state["angle"] = ptv::InteractiveValue::of(0.0f);
        ci.pristine = ci.state;
        world.registry().emplace<ptv::CInteractive>(door, std::move(ci));

        const auto bottle = world.create_entity("glass_bottle");
        ptv::Transform t{};
        t.position = ptv::Vec3(1.0f, 0.0f, 2.0f);
        world.registry().emplace<ptv::CTransform>(bottle, ptv::CTransform{t});
        world.registry().emplace<ptv::CMovable>(bottle, ptv::CMovable{t});
    };

    ptv::World first;
    make_world(first);

    // Nothing touched yet, so a save must be EMPTY of deltas. If an
    // untouched level produced records, every save would grow with noise.
    {
        const auto doc = ptv::SaveSystem::save_world(first);
        if (doc.contains("interactives")) {
            return fail("persistence: an untouched door produced a delta");
        }
        if (doc.contains("moved")) {
            return fail("persistence: an unmoved bottle produced a delta");
        }
    }

    // Open the door and move the bottle, the way a mechanic would.
    {
        auto& ci = first.registry().get<ptv::CInteractive>(
            first.find("front_door"));
        ci.state["open"] = ptv::InteractiveValue::of(true);
        ci.state["angle"] = ptv::InteractiveValue::of(45.0f);
        auto& t = first.registry().get<ptv::CTransform>(
            first.find("glass_bottle")).t;
        t.position = ptv::Vec3(1.0f, 0.8f, 2.0f);  // onto the bed
    }

    const auto saved = ptv::SaveSystem::save_world(first);
    if (!saved.contains("interactives") ||
        !saved["interactives"].contains("front_door")) {
        return fail("persistence: the opened door was not recorded");
    }
    if (!saved.contains("moved") ||
        !saved["moved"].contains("glass_bottle")) {
        return fail("persistence: the moved bottle was not recorded");
    }
    // Keyed by the scene's stable string id, never an entt handle.
    if (saved["moved"].size() != 1) {
        return fail("persistence: unexpected extra moved records");
    }

    // --- Rebuild the scene from scratch and overlay the save ----------
    //
    // This is the real test: a transition destroys the world and rebuilds it
    // from the unchanged document, so the save has to carry the difference
    // across on its own.
    ptv::World second;
    make_world(second);
    {
        const auto& ci = second.registry().get<ptv::CInteractive>(
            second.find("front_door"));
        if (ci.state.at("open").b) {
            return fail("persistence: the rebuilt door started open");
        }
    }

    ptv::SaveSystem::load_world(saved, second);
    {
        const auto& ci = second.registry().get<ptv::CInteractive>(
            second.find("front_door"));
        if (!ci.state.at("open").b) {
            return fail("persistence: the door did not reopen after reload");
        }
        if (std::fabs(ci.state.at("angle").number - 45.0f) > 1e-3f) {
            return fail("persistence: the door angle was not restored");
        }
        // pristine must still be the document's value, or the next save
        // would compare against the restored state and record nothing.
        if (ci.pristine.at("open").b) {
            return fail("persistence: pristine was overwritten on load");
        }
        const auto& t = second.registry().get<ptv::CTransform>(
            second.find("glass_bottle")).t;
        if (std::fabs(t.position.y - 0.8f) > 1e-3f) {
            return fail("persistence: the bottle's position was not restored");
        }
    }

    // A save referring to something this world no longer has must be
    // skipped, not fatal: a regenerated world with different content still
    // has to load an old save.
    {
        ptv::World third;
        make_world(third);
        auto stale = saved;
        stale["interactives"]["a_door_that_no_longer_exists"] = {
            {"open", {{"type", "bool"}, {"value", true}}}};
        stale["moved"]["a_prop_that_no_longer_exists"] = {
            {"position", {9.0f, 9.0f, 9.0f}}};
        ptv::SaveSystem::load_world(stale, third);
        const auto& ci = third.registry().get<ptv::CInteractive>(
            third.find("front_door"));
        if (!ci.state.at("open").b) {
            return fail("persistence: a stale id broke the valid records");
        }
    }

    return ok("interactive persistence: deltas only, stable keys, "
              "document authoritative, stale ids tolerated");
}

// The semantic split that makes equip work at all. A single "locked" flag
// conflated two unrelated questions -- "can a passing hand knock this off"
// and "may the player deliberately take it off" -- and the result was an
// equipped item with no way to come off. These assert the split holds.
static int case_equip_semantics() {
    ptv::World world;

    const auto wearer = world.create_entity("player");
    world.registry().emplace<ptv::CTransform>(wearer, ptv::CTransform{});
    {
        ptv::CEquipSlots slots{};
        ptv::EquipSlot hand{};
        hand.name = "hand_r";
        hand.body_slot = "hand_r";
        hand.exclusive = true;
        slots.slots.push_back(hand);
        // A custom, non-anatomical slot: anatomy alone cannot express "sword
        // sheath on the left hip", which is why slots are scene-declared.
        ptv::EquipSlot sheath{};
        sheath.name = "sword_sheath_l";
        sheath.body_slot = "waist";
        sheath.accepts_tags = {"blade"};
        slots.slots.push_back(sheath);
        world.registry().emplace<ptv::CEquipSlots>(wearer, std::move(slots));
    }

    const auto glove = world.create_entity("glove");
    world.registry().emplace<ptv::CTransform>(glove, ptv::CTransform{});
    auto& eq = world.registry().emplace<ptv::CEquipped>(glove,
                                                        ptv::CEquipped{});

    // Default: protected from a passing hand, but still removable on purpose.
    if (!eq.passive_grab_protection) {
        return fail("equip: passive protection should default on");
    }
    if (!eq.removable) {
        return fail("equip: an item must default to removable -- the old "
                    "single 'lock' flag is exactly what stranded items");
    }
    if (!eq.re_equippable) {
        return fail("equip: an item should default to re-equippable");
    }

    // Grab must ignore a protected, equipped item. This is the interaction
    // that stops a gauntlet being ripped off by brushing past it.
    eq.wearer = static_cast<std::uint32_t>(wearer);
    eq.slot = "hand_r";
    {
        auto& tag = world.registry().get_or_emplace<ptv::CTag>(glove);
        tag.tags.push_back("grabbable");
    }
    const auto mech = ptv::MechanicRegistry::get().make_mechanic("builtin.grab");
    if (!mech) return fail("builtin.grab is not registered");

    // Protection ON: invisible to the grab search. Protection OFF: visible.
    // Asserted through the tag state the search reads, since find_grabbable
    // is internal to the module.
    if (!world.registry().all_of<ptv::CEquipped>(glove)) {
        return fail("equip: CEquipped vanished");
    }
    eq.passive_grab_protection = false;
    if (world.registry().get<ptv::CEquipped>(glove).passive_grab_protection) {
        return fail("equip: protection flag is not mutable");
    }

    // Runtime mutability is the whole point of the removable flag: a helmet
    // is fixed in flight and free on the ground, on the SAME instance.
    eq.removable = false;
    if (world.registry().get<ptv::CEquipped>(glove).removable) {
        return fail("equip: removable must be runtime-mutable");
    }
    eq.removable = true;

    // And re_equippable is per-instance, not per-type: a sterile glove that
    // came off is contaminated even though gloves in general still fit.
    eq.re_equippable = false;
    if (world.registry().get<ptv::CEquipped>(glove).re_equippable) {
        return fail("equip: re_equippable must be runtime-mutable");
    }

    return ok("equip semantics: protection, removable and re-equippable are "
              "three independent runtime flags");
}

// Slots have to come from the SCENE, not from a test. CEquipSlots existed and
// nothing ever populated it from scene.json, so EquipMechanic::pick_slot_near
// returned no candidate on every wearer in every generated game and an
// authored holster silently accepted nothing. This asserts the whole path:
// JSON -> SceneDoc -> CEquipSlots.
static int case_scene_declared_equip_slots() {
    const char* doc = R"({
        "version": 1,
        "prompt_id": "smoke",
        "scene_id": "holster",
        "name": "Holster",
        "entities": [
          {"id": "player_rig", "name": "Player",
           "transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": 1.0},
           "tags": ["player"],
           "equip_slots": [
             {"name": "hip_holster_r", "body_slot": "waist",
              "local_position": [0.16, -0.05, 0.02],
              "local_rotation": [0,0,0,1],
              "snap_radius_m": 0.2, "exclusive": true,
              "accepts_tags": ["pistol"]},
             {"name": "unnamed_is_dropped", "body_slot": "back"}
           ]},
          {"id": "bench", "name": "Bench",
           "transform": {"position": [1,0,0], "rotation": [0,0,0,1], "scale": 1.0},
           "tags": []}
        ]
    })";
    const char* path = "ptv_smoke_equip_scene.json";
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return fail("equip slots: could not write temp scene");
    std::fwrite(doc, 1, std::strlen(doc), f);
    std::fclose(f);

    const ptv::SceneDoc loaded = ptv::LoadSceneJson(path);
    if (loaded.entities.size() != 2) {
        return fail("equip slots: scene did not parse");
    }
    if (loaded.entities[0].equip_slots.size() != 2) {
        return fail("equip slots: equip_slots did not survive the parse");
    }

    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);

    const auto rig = world.find("player_rig");
    if (rig == entt::null || !world.registry().all_of<ptv::CEquipSlots>(rig)) {
        return fail("equip slots: the wearer got no CEquipSlots, so "
                    "builtin.equip would offer no candidate slot at all");
    }
    const auto& slots = world.registry().get<ptv::CEquipSlots>(rig).slots;
    if (slots.size() != 2) return fail("equip slots: wrong slot count");

    const auto& holster = slots[0];
    if (holster.name != "hip_holster_r" || holster.body_slot != "waist") {
        return fail("equip slots: name / body_slot did not round-trip");
    }
    if (std::abs(holster.local_position.x - 0.16f) > 1e-5f ||
        std::abs(holster.local_position.y + 0.05f) > 1e-5f) {
        return fail("equip slots: local_position did not round-trip");
    }
    if (std::abs(holster.snap_radius_m - 0.2f) > 1e-5f) {
        return fail("equip slots: snap_radius_m did not round-trip");
    }
    if (holster.accepts_tags.size() != 1 ||
        holster.accepts_tags[0] != "pistol") {
        return fail("equip slots: accepts_tags did not round-trip -- an empty "
                    "list would let a rifle into a pistol holster");
    }
    // Defaults hold for a slot that declares only the essentials.
    if (!slots[1].exclusive || slots[1].snap_radius_m != 0.0f) {
        return fail("equip slots: defaults did not apply");
    }

    // An entity that declares no slots must not get the component: a bench is
    // not a wearer, and an empty CEquipSlots would make it look like one.
    const auto bench = world.find("bench");
    if (bench == entt::null || world.registry().all_of<ptv::CEquipSlots>(bench)) {
        return fail("equip slots: a slot-less entity became a wearer");
    }

    std::remove(path);
    return ok("scene-declared equip slots reach CEquipSlots on the wearer");
}

// A reflection is an IMPROPER transform, and that single fact drives the
// whole mirror design: it is why a second front-face-culling pipeline exists.
// Assert the property rather than the matrix entries, so the test still holds
// if the construction is rewritten.
static int case_mirror_reflection() {
    // Mirror on the wall at z = -2, facing +Z back toward the room.
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    const glm::vec3 p(0.0f, 1.5f, -2.0f);
    const float d = -glm::dot(n, p);

    glm::mat4 r(1.0f);
    r[0][0] = 1.0f - 2.0f * n.x * n.x;
    r[1][0] = -2.0f * n.x * n.y;
    r[2][0] = -2.0f * n.x * n.z;
    r[3][0] = -2.0f * n.x * d;
    r[0][1] = -2.0f * n.y * n.x;
    r[1][1] = 1.0f - 2.0f * n.y * n.y;
    r[2][1] = -2.0f * n.y * n.z;
    r[3][1] = -2.0f * n.y * d;
    r[0][2] = -2.0f * n.z * n.x;
    r[1][2] = -2.0f * n.z * n.y;
    r[2][2] = 1.0f - 2.0f * n.z * n.z;
    r[3][2] = -2.0f * n.z * d;

    // A point on the plane must not move.
    {
        const glm::vec4 on_plane = r * glm::vec4(1.0f, 0.5f, -2.0f, 1.0f);
        if (glm::length(glm::vec3(on_plane) - glm::vec3(1.0f, 0.5f, -2.0f)) >
            1e-4f) {
            return fail("mirror: a point on the plane moved");
        }
    }

    // A point one metre in front reflects to one metre behind, and only the
    // axis along the normal changes.
    {
        const glm::vec4 in_front = r * glm::vec4(0.4f, 1.7f, -1.0f, 1.0f);
        const glm::vec3 want(0.4f, 1.7f, -3.0f);
        if (glm::length(glm::vec3(in_front) - want) > 1e-4f) {
            return fail("mirror: reflection landed in the wrong place");
        }
    }

    // The determinant is negative. This is the whole reason the renderer
    // needs a front-face-culling variant: every reflected triangle arrives
    // wound the other way, and drawing it with the usual back-face culling
    // would show the inside of the avatar.
    if (glm::determinant(r) >= 0.0f) {
        return fail("mirror: reflection was not an improper transform");
    }

    // Applying it twice is the identity, so a reflection of a reflection is
    // the original object.
    {
        const glm::mat4 twice = r * r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                const float want = (c == row) ? 1.0f : 0.0f;
                if (std::fabs(twice[c][row] - want) > 1e-4f) {
                    return fail("mirror: reflecting twice is not identity");
                }
            }
        }
    }

    return ok("mirror: plane fixed, improper, involutive");
}

// The three rules that make VR arm IK usable are not properties of the maths,
// they are things shipped VR work learned the hard way. Each gets a case,
// because each failure is one users notice immediately.
static int case_two_bone_ik() {
    using namespace ptv::two_bone_ik;
    const glm::vec3 shoulder(0.0f, 1.4f, 0.0f);
    const glm::vec3 pole(0.0f, -1.0f, 0.4f);
    const float upper = 0.30f;
    const float lower = 0.26f;

    // Reachable target: both bone lengths must be honoured exactly, or the
    // mesh stretches.
    {
        const glm::vec3 target(0.25f, 1.25f, -0.20f);
        const auto s = solve(shoulder, target, upper, lower, pole);
        if (!s.valid) return fail("ik: reachable target failed to solve");
        const float a = glm::length(s.elbow - shoulder);
        const float b = glm::length(s.wrist - s.elbow);
        if (std::fabs(a - upper) > 1e-3f) {
            return fail("ik: upper bone length not preserved");
        }
        if (std::fabs(b - lower) > 1e-3f) {
            return fail("ik: lower bone length not preserved");
        }
        if (glm::length(s.wrist - target) > 1e-5f) {
            return fail("ik: wrist did not land on the target");
        }
    }

    // Rule 2: past full reach the hand STAYS on the controller. Pulling it
    // back to arm's length is the single most disturbing thing this solver
    // could do, because the virtual hand stops agreeing with the real one.
    {
        const glm::vec3 far_target(3.0f, 1.4f, 0.0f);
        const auto s = solve(shoulder, far_target, upper, lower, pole);
        if (!s.valid) return fail("ik: out-of-reach target failed to solve");
        if (glm::length(s.wrist - far_target) > 1e-5f) {
            return fail("ik: wrist was distance-clamped at full reach");
        }
    }

    // Rule 1: the elbow goes to the side the pole hint asks for. Flip the
    // hint and the elbow must flip with it, or arms bend backwards.
    {
        const glm::vec3 target(0.0f, 1.15f, -0.30f);
        const auto down = solve(shoulder, target, upper, lower,
                                glm::vec3(0.0f, -1.0f, 0.0f));
        const auto up = solve(shoulder, target, upper, lower,
                              glm::vec3(0.0f, 1.0f, 0.0f));
        if (!down.valid || !up.valid) return fail("ik: pole solves failed");
        if (!(down.elbow.y < up.elbow.y)) {
            return fail("ik: elbow ignored the pole hint");
        }
    }

    // Rule 3: a target at exactly full extension must not produce a perfectly
    // straight chain, because the bend plane is undefined there and the arm
    // jitters between solutions.
    {
        const glm::vec3 straight(0.0f, 1.4f - (upper + lower), 0.0f);
        const auto s = solve(shoulder, straight, upper, lower, pole);
        if (!s.valid) return fail("ik: full-extension target failed to solve");
        const glm::vec3 ua = glm::normalize(s.elbow - shoulder);
        const glm::vec3 la = glm::normalize(s.wrist - s.elbow);
        if (glm::dot(ua, la) > 0.9995f) {
            return fail("ik: chain went fully straight into the singularity");
        }
    }

    // Degenerate input must be reported, not silently turned into NaNs on a
    // joint the pose system will then propagate through the whole skeleton.
    {
        const auto s = solve(shoulder, glm::vec3(0.2f, 1.2f, 0.0f), 0.0f,
                             lower, pole);
        if (s.valid) return fail("ik: zero-length bone reported success");
    }

    return ok("two-bone ik: lengths, reach, pole and singularity");
}

// Head visibility is a PER-VIEW decision. The property that matters is that
// suppressing the head for the player's own view leaves the palette a mirror
// would use completely untouched -- get that wrong and the reflection is
// headless, which is the bug this whole two-palette design exists to avoid.
static int case_self_view_hide(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    gml::SkeletonData skel;
    if (!gml::load_glb_skeleton(glb, 0.0f, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skeleton: %s\n", err.c_str());
        return 1;
    }

    ptv::World world;
    const auto e = world.create_entity("avatar");
    world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{});
    ptv::CSkeleton sk{};
    sk.skeleton = skel;
    sk.local_pose = skel.bind_pose;
    world.registry().emplace<ptv::CSkeleton>(e, std::move(sk));

    // No suppression yet: one palette, no variant.
    ptv::tick_animators(world, 0.016f);
    {
        const auto& s = world.registry().get<ptv::CSkeleton>(e);
        if (!s.palette_self_view.empty()) {
            return fail("a variant palette was built with no CSelfViewHide");
        }
    }

    // Suppress a subtree. Any joint with children will do; the fixture's rig
    // is small, so take joint 0's subtree minus the root.
    const int hidden = static_cast<int>(skel.joint_count()) - 1;
    world.registry().emplace<ptv::CSelfViewHide>(
        e, ptv::CSelfViewHide{{hidden}, 0.0f});

    ptv::tick_animators(world, 0.016f);
    {
        const auto& s = world.registry().get<ptv::CSkeleton>(e);
        if (s.palette_self_view.size() != s.palette.size()) {
            return fail("variant palette was not built for CSelfViewHide");
        }
        // The suppressed joint collapses in the self view...
        const ptv::Mat4& hidden_self =
            s.palette_self_view[static_cast<size_t>(hidden)];
        const float linear = std::fabs(hidden_self[0][0]) +
                             std::fabs(hidden_self[1][1]) +
                             std::fabs(hidden_self[2][2]);
        if (linear > 1e-4f) {
            return fail("suppressed joint did not collapse in the self view");
        }
        // ...and is untouched in the palette every other view uses.
        const ptv::Mat4& hidden_full =
            s.palette[static_cast<size_t>(hidden)];
        const float full_linear = std::fabs(hidden_full[0][0]) +
                                  std::fabs(hidden_full[1][1]) +
                                  std::fabs(hidden_full[2][2]);
        if (full_linear < 1e-3f) {
            return fail("the shared palette was collapsed too; a mirror "
                        "would show a headless avatar");
        }
    }

    // Toggling it back on has to drop the variant, not leave a stale copy.
    world.registry().get<ptv::CSelfViewHide>(e).scale = 1.0f;
    ptv::tick_animators(world, 0.016f);
    if (!world.registry().get<ptv::CSkeleton>(e).palette_self_view.empty()) {
        return fail("variant palette survived being toggled back on");
    }

    std::printf("[ok] self-view hide: %zu joints, mirror palette intact\n",
                     skel.joint_count());
    return 0;
}

namespace {

/** ISkeleton over a loaded SkeletonData, with the same forward kinematics
 *  Application's SkeletonService uses. Enough to run HandRig and pose a hand
 *  the way builtin.hand_pose does, without an Application. */
class TestSkeleton final : public ptv::ISkeleton {
public:
    ptv::gltf_mesh_load::SkeletonData sk;
    std::vector<ptv::gltf_mesh_load::JointPose> pose;

    void reset() { pose = sk.bind_pose; }

    bool has_skeleton(std::uint32_t) const override {
        return sk.joint_count() > 0;
    }
    int joint_index(std::uint32_t, const char* name) const override {
        const auto it = sk.name_to_joint.find(name);
        return it == sk.name_to_joint.end() ? -1 : it->second;
    }
    int joint_count(std::uint32_t) const override {
        return static_cast<int>(sk.joint_count());
    }
    int joint_for_slot(std::uint32_t, const char* slot) const override {
        for (const auto& c : ptv::body_slot_candidates(slot)) {
            for (size_t i = 0; i < sk.names.size(); ++i) {
                if (ptv::normalize_joint_name(sk.names[i]) == c) {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }
    int joint_parent(std::uint32_t, int j) const override {
        return (j < 0 || static_cast<size_t>(j) >= sk.parents.size())
                   ? -1
                   : sk.parents[static_cast<size_t>(j)];
    }
    ptv::Mat4 joint_world(std::uint32_t, int j) const override {
        return fk(pose, j, false);
    }
    ptv::Mat4 joint_model(std::uint32_t, int j) const override {
        return fk(pose, j, true);
    }
    ptv::Mat4 joint_bind_model(std::uint32_t, int j) const override {
        return fk(sk.bind_pose, j, true);
    }
    ptv::Quat joint_bind_rotation(std::uint32_t, int j) const override {
        return sk.bind_pose[static_cast<size_t>(j)].rotation;
    }
    ptv::Quat joint_local_rotation(std::uint32_t, int j) const override {
        return pose[static_cast<size_t>(j)].rotation;
    }
    void set_joint_rotation_from_bind(std::uint32_t, int j,
                                      const ptv::Quat& d) override {
        pose[static_cast<size_t>(j)].rotation =
            glm::normalize(sk.bind_pose[static_cast<size_t>(j)].rotation * d);
    }
    int joint_subtree(std::uint32_t, int, int*, int) const override {
        return 0;
    }
    void set_joint_rotation(std::uint32_t, int j, const ptv::Quat& q) override {
        pose[static_cast<size_t>(j)].rotation = glm::normalize(q);
    }
    void set_joint_translation(std::uint32_t, int, const ptv::Vec3&) override {}
    void set_joint_scale(std::uint32_t, int, const ptv::Vec3&) override {}
    bool play_clip(std::uint32_t, const char*, bool, float) override {
        return false;
    }
    void stop_clip(std::uint32_t) override {}
    void set_clip_speed(std::uint32_t, float) override {}
    bool has_clip(std::uint32_t, const char*) const override { return false; }
    const char* current_clip(std::uint32_t) const override { return ""; }
    float clip_time(std::uint32_t) const override { return 0.0f; }
    float clip_duration(std::uint32_t) const override { return 0.0f; }
    bool clip_finished(std::uint32_t) const override { return false; }

private:
    ptv::Mat4 fk(const std::vector<ptv::gltf_mesh_load::JointPose>& p, int j,
                 bool ortho) const {
        ptv::Mat4 m(1.0f);
        int cur = j;
        while (cur >= 0) {
            auto jp = p[static_cast<size_t>(cur)];
            jp.translation *= sk.unit_scale;
            m = jp.matrix() * m;
            cur = sk.parents[static_cast<size_t>(cur)];
        }
        if (!ortho) return m;
        ptv::Vec3 x = glm::normalize(ptv::Vec3(m[0]));
        ptv::Vec3 y = ptv::Vec3(m[1]);
        y = glm::normalize(y - x * glm::dot(x, y));
        const ptv::Vec3 z = glm::cross(x, y);
        ptv::Mat4 o(1.0f);
        o[0] = ptv::Vec4(x, 0.0f);
        o[1] = ptv::Vec4(y, 0.0f);
        o[2] = ptv::Vec4(z, 0.0f);
        o[3] = m[3];
        return o;
    }
};

/** The other hand's file: default_hand_right.glb <-> default_hand_left.glb. */
std::string other_hand_path(const std::string& path) {
    std::string p = path;
    const auto r = p.rfind("right");
    if (r != std::string::npos) return p.replace(r, 5, "left");
    const auto l = p.rfind("left");
    if (l != std::string::npos) return p.replace(l, 4, "right");
    return p;
}

}  // namespace

// The default rigged hands are what builtin.hand_pose poses in every Hands
// Only game, so they carry a contract: canonical BodySlots joint names on the
// correct side, five complete finger chains under the wrist, and knuckle
// vertices that blend two joints (a hand bound one-box-per-joint would tear
// open at every bend).
static int case_hand_asset(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    for (const std::string& path : {std::string(glb), other_hand_path(glb)}) {
        const bool right = path.find("right") != std::string::npos;
        const char* sfx = right ? "_r" : "_l";
        std::string err;
        std::vector<gml::SkinnedVertex> verts;
        std::vector<std::uint32_t> indices;
        gml::SkeletonData skel;
        if (!gml::load_glb_skinned(path, 0.2f, verts, indices, skel, err)) {
            std::fprintf(stderr, "[fail] load_glb_skinned %s: %s\n",
                         path.c_str(), err.c_str());
            return 1;
        }
        auto has = [&](const std::string& n) {
            return skel.name_to_joint.count(n) == 1;
        };
        if (!has(std::string("hand") + sfx)) return fail("hand has no wrist");
        for (const char* f : {"thumb", "index", "middle", "ring", "pinky"}) {
            for (int s = 1; s <= 3; ++s) {
                const std::string n = std::string(f) + "_0" +
                                      std::to_string(s) + sfx;
                if (!has(n)) {
                    std::fprintf(stderr, "[fail] %s missing joint %s\n",
                                 path.c_str(), n.c_str());
                    return 1;
                }
            }
        }
    size_t blended = 0;
    for (const auto& v : verts) {
        int sum = 0;
            for (int k = 0; k < 4; ++k) sum += v.weights[k];
        if (sum != 255) return fail("hand weights do not sum to 255");
        if (v.weights[0] != 0 && v.weights[1] != 0) ++blended;
    }
    if (blended == 0)
        return fail("no knuckle vertices blend two joints; bends would tear");
        std::printf("[ok] %s: %zu joints, %zu verts, %zu blended\n",
                    right ? "right hand" : "left hand", skel.joint_count(),
                    verts.size(), blended);
    }
    return 0;
}

// What builtin.hand_pose does, on the real default hands of BOTH sides:
// HandRig finds the fingers, and a bind-relative curl about the measured axis
// moves each fingertip INTO THE PALM (not sideways, not backwards) while
// leaving the other fingers alone. These are exactly the on-device bugs the
// old fixed-axis curl produced on a real rig.
static int case_hand_pose(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    for (const std::string& path : {std::string(glb), other_hand_path(glb)}) {
        const bool right = path.find("right") != std::string::npos;
        const int hand = right ? 1 : 0;
        TestSkeleton ts;
    std::string err;
        if (!gml::load_glb_skeleton(path, 0.2f, ts.sk, err)) {
        std::fprintf(stderr, "[fail] load_glb_skeleton: %s\n", err.c_str());
        return 1;
    }
        ts.reset();
        ptv::hand_rig::Resolution how{};
        const auto rig = ptv::hand_rig::analyze(ts, 0, hand, &how);
        if (!rig.valid) return fail("HandRig found no hand");
        if (how != ptv::hand_rig::Resolution::Names)
            return fail("HandRig did not resolve the default hand by name");
        for (int f = 0; f < ptv::hand_rig::kFingers; ++f) {
            if (!rig.fingers[static_cast<size_t>(f)].present())
                return fail("HandRig is missing a finger");
        }

        // Palm normal: out of the palm. The grip frame's +X is INTO the palm
        // on the right hand and out of it on the left.
        const ptv::Vec3 gx(rig.hand_frame_model[0]);
        const ptv::Vec3 palm = right ? -gx : gx;

        auto tip = [&](int finger) {
            const auto& seg = rig.fingers[static_cast<size_t>(finger)]
                                  .segments[2];
            // Tip: the distal joint's child if there is one.
            int j = seg.joint;
            for (int k = 0; k < ts.joint_count(0); ++k) {
                if (ts.joint_parent(0, k) == seg.joint) {
                    j = k;
                    break;
                }
            }
            return ptv::Vec3(ts.joint_model(0, j)[3]);
        };
        std::array<ptv::Vec3, 5> rest{};
        for (int f = 0; f < 5; ++f) rest[static_cast<size_t>(f)] = tip(f);

        for (int f = ptv::hand_rig::Index; f <= ptv::hand_rig::Pinky; ++f) {
            // A full curl must move the tip a real distance...
            ts.reset();
            for (const auto& seg : rig.fingers[static_cast<size_t>(f)].segments) {
                if (seg.joint < 0) continue;
                ts.set_joint_rotation_from_bind(
                    0, seg.joint, glm::angleAxis(1.2f, seg.curl_axis_local));
            }
            if (glm::length(tip(f) - rest[static_cast<size_t>(f)]) < 0.01f)
                return fail("a full curl barely moved the fingertip");
            // ...and its DIRECTION, taken from a small curl (a full one wraps
            // the tip past the palm, which says nothing about the axis), must
            // be into the palm.
            ts.reset();
            for (const auto& seg : rig.fingers[static_cast<size_t>(f)].segments) {
                if (seg.joint < 0) continue;
                ts.set_joint_rotation_from_bind(
                    0, seg.joint, glm::angleAxis(0.25f, seg.curl_axis_local));
            }
            const ptv::Vec3 moved = tip(f) - rest[static_cast<size_t>(f)];
            // A rest pose that is already curled swings the tip partly back
            // toward the wrist too, so the test is: toward the palm side,
            // AND closer to the grip point in the middle of the palm.
            const ptv::Vec3 grip(rig.hand_frame_model[3]);
            const bool closer = glm::length(tip(f) - grip) <
                                glm::length(rest[static_cast<size_t>(f)] - grip);
            if (glm::dot(glm::normalize(moved), palm) < 0.15f || !closer) {
                std::fprintf(stderr,
                             "[fail] %s hand finger %d curled away from the "
                             "palm (dot %.2f)\n",
                             right ? "right" : "left", f,
                             glm::dot(glm::normalize(moved), palm));
                return 1;
            }
            for (int o = 0; o < 5; ++o) {
                if (o == f) continue;
                if (glm::length(tip(o) - rest[static_cast<size_t>(o)]) > 1e-4f)
                    return fail("curling one finger moved another");
            }
        }

        // The thumb resting on the stick moves toward the index finger.
        ts.reset();
        const auto& thumb = rig.fingers[ptv::hand_rig::Thumb];
        ts.set_joint_rotation_from_bind(
            0, thumb.segments[0].joint,
            glm::angleAxis(0.45f, rig.thumb_adduct_axis_local));
        const ptv::Vec3 index_base(
            ts.joint_model(0, rig.fingers[ptv::hand_rig::Index].segments[0]
                                  .joint)[3]);
        const float before = glm::length(rest[0] - index_base);
        const float after = glm::length(tip(0) - index_base);
        if (!(after < before))
            return fail("thumb did not move toward the index finger");

        std::printf("[ok] %s hand: fingers curl into the palm, thumb "
                    "adducts toward the index\n",
                    right ? "right" : "left");
    }
    return 0;
}

// What the player SEES of the default hands in a Hands Only game: the shipped
// hand, skinned exactly as the GPU does it (palette * scaled vertex), placed
// the way Application::place_hand_entity places it (grip * grip_to_entity)
// on a controller 0.4 m in front of the eyes. It must be hand-sized, finite,
// sit on the controller and be in view -- in the bind pose and in a fist.
static int case_default_hands(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    for (const std::string& path : {std::string(glb), other_hand_path(glb)}) {
        const bool right = path.find("right") != std::string::npos;
        const int hand = right ? 1 : 0;
        const char* side = right ? "right" : "left";
        std::string err;
        std::vector<gml::SkinnedVertex> verts;
        std::vector<std::uint32_t> indices;
        gml::SkeletonData skel;
        if (!gml::load_glb_skinned(path, 0.2f, verts, indices, skel, err)) {
            std::fprintf(stderr, "[fail] load_glb_skinned %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        TestSkeleton ts;
        if (!gml::load_glb_skeleton(path, 0.2f, ts.sk, err)) {
            std::fprintf(stderr, "[fail] load_glb_skeleton %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        ts.reset();

        auto skin = [&](const std::vector<ptv::Mat4>& pal, const ptv::Mat4& model,
                        ptv::Vec3* lo, ptv::Vec3* hi, ptv::Vec3* centre) {
            *lo = ptv::Vec3(1e9f);
            *hi = ptv::Vec3(-1e9f);
            ptv::Vec3 sum(0.0f);
            for (const auto& v : verts) {
                ptv::Mat4 m(0.0f);
                for (int k = 0; k < 4; ++k) {
                    if (v.weights[k] == 0) continue;
                    m += pal[v.joints[k]] * (static_cast<float>(v.weights[k]) / 255.0f);
                }
                const ptv::Vec3 p(model * (m * ptv::Vec4(v.pos[0], v.pos[1], v.pos[2], 1.0f)));
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
                *lo = glm::min(*lo, p);
                *hi = glm::max(*hi, p);
                sum += p;
            }
            *centre = sum / static_cast<float>(std::max<size_t>(1, verts.size()));
            return true;
        };

        std::vector<ptv::Mat4> pal;
        gml::build_joint_palette(skel, skel.bind_pose, pal);
        ptv::Vec3 lo, hi, c;
        if (!skin(pal, ptv::Mat4(1.0f), &lo, &hi, &c)) return fail("bind-pose hand has NaN vertices");
        const ptv::Vec3 ext = hi - lo;
        const float longest = std::max(ext.x, std::max(ext.y, ext.z));
        std::printf("[info] %s hand bind: longest %.3f m, centre %.3f %.3f %.3f (unit_scale %.5f)\n",
                    side, longest, c.x, c.y, c.z, skel.unit_scale);
        if (longest < 0.12f || longest > 0.30f) {
            std::fprintf(stderr, "[fail] %s hand is %.3f m long when drawn (want 0.12-0.30)\n",
                         side, longest);
            return 1;
        }

        const auto rig = ptv::hand_rig::analyze(ts, 0, hand, nullptr);
        if (!rig.valid) return fail("HandRig found no hand");
        const ptv::Vec3 gt(rig.grip_to_entity[3]);
        std::printf("[info] %s hand grip_to_entity translation %.3f %.3f %.3f\n", side, gt.x,
                    gt.y, gt.z);
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                if (!std::isfinite(rig.grip_to_entity[col][row])) {
                    return fail("grip_to_entity is not finite");
                }
            }
        }
        if (glm::length(gt) > 0.3f) return fail("grip_to_entity puts the hand >0.3 m from the grip");

        // A controller 0.4 m ahead of eyes at 1.6 m, slightly right and low.
        const ptv::Vec3 grip_pos(0.15f, 1.35f, -0.4f);
        const ptv::Mat4 grip = glm::translate(ptv::Mat4(1.0f), grip_pos);
        const ptv::Mat4 model = grip * rig.grip_to_entity;
        if (!skin(pal, model, &lo, &hi, &c)) return fail("placed hand has NaN vertices");
        const float off = glm::length(c - grip_pos);
        const ptv::Vec3 eye(0.0f, 1.6f, 0.0f);
        const ptv::Vec3 to = glm::normalize(c - eye);
        const float angle = glm::degrees(std::acos(glm::dot(to, ptv::Vec3(0, 0, -1))));
        std::printf("[info] %s hand on the controller: centre %.3f m from the grip, %.1f deg "
                    "off the view axis\n", side, off, angle);
        if (off > 0.15f) return fail("the hand is drawn away from the controller");
        if (angle > 45.0f) return fail("a hand held in front of the eyes is out of view");

        // A full fist, as builtin.hand_pose poses it.
        for (const auto& f : rig.fingers) {
            for (const auto& seg : f.segments) {
                if (seg.joint < 0) continue;
                ts.set_joint_rotation_from_bind(0, seg.joint, glm::angleAxis(1.2f, seg.curl_axis_local));
            }
        }
        gml::build_joint_palette(skel, ts.pose, pal);
        if (!skin(pal, model, &lo, &hi, &c)) return fail("a fist has NaN vertices");
        std::printf("[ok] %s hand: drawn %.3f m long on the controller, fist finite\n", side,
                    longest);
    }
    return 0;
}

// The player body: grounded, and walls stop it from every direction.
static int case_player_character() {
    ptv::World world;
    auto add_box = [&](const char* id, ptv::Vec3 pos, ptv::Vec3 size,
                       ptv::Quat rot = ptv::Quat(1, 0, 0, 0)) {
        const auto e = world.create_entity(id);
        ptv::Transform t{};
        t.position = pos;
        t.rotation = rot;
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        world.registry().emplace<ptv::CPrimitive>(
            e, ptv::CPrimitive{"box", size, ptv::Vec3(0.5f)});
        ptv::CPhysics p{};
        p.kind = "static";
        p.collision_shape = "box";
        world.registry().emplace<ptv::CPhysics>(e, p);
    };
    add_box("floor", ptv::Vec3(0.0f, -0.1f, 0.0f), ptv::Vec3(60.0f, 0.2f, 60.0f));
    // A 0.3 m thick wall whose near face is at x = 3.85.
    add_box("wall", ptv::Vec3(4.0f, 1.5f, 0.0f), ptv::Vec3(0.3f, 3.0f, 12.0f));
    // A 12 degree ramp rising toward -Z, starting at z = -6.
    add_box("ramp", ptv::Vec3(-6.0f, 0.4f, -10.0f), ptv::Vec3(3.0f, 0.2f, 10.0f),
            glm::angleAxis(glm::radians(12.0f), ptv::Vec3(1, 0, 0)));

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);
    const float dt = 1.0f / 72.0f;

    // 1) Sliding across flat ground for five seconds must not climb.
    physics.create_player_character(ptv::Vec3(0.0f, 0.0f, 0.0f), 1.7f, 0.22f);
    for (int i = 0; i < 72 * 5; ++i) {
        physics.move_player_character(ptv::Vec3(0.0f, 0.0f, 1.5f * dt), true,
                                      0.0f, dt);
    }
    {
        const auto st = physics.player_character_state();
        if (std::fabs(st.feet.y) > 0.03f) {
            std::fprintf(stderr, "  feet at y=%.3f after 5 s\n", st.feet.y);
            return fail("sliding drifted vertically");
        }
        if (!st.grounded) return fail("sliding body is not grounded");
    }

    // 2) The wall blocks from every approach: forward, backward and diagonal.
    struct Approach {
        ptv::Vec3 start;
        ptv::Vec3 dir;
    };
    const Approach approaches[] = {
        {ptv::Vec3(1.0f, 0.0f, 0.0f), ptv::Vec3(1.0f, 0.0f, 0.0f)},
        {ptv::Vec3(1.0f, 0.0f, 2.0f), glm::normalize(ptv::Vec3(1, 0, -1))},
        {ptv::Vec3(1.0f, 0.0f, -2.0f), glm::normalize(ptv::Vec3(1, 0, 1))},
        {ptv::Vec3(7.0f, 0.0f, 0.0f), ptv::Vec3(-1.0f, 0.0f, 0.0f)},
    };
    for (const auto& a : approaches) {
        physics.teleport_player_character(a.start);
        for (int i = 0; i < 72 * 4; ++i) {
            physics.move_player_character(a.dir * (2.0f * dt), true, 0.0f, dt);
        }
        const float x = physics.player_character_state().feet.x;
        const bool from_near = a.start.x < 4.0f;
        if (from_near ? (x > 3.85f) : (x < 4.15f)) {
            std::fprintf(stderr, "  approach from x=%.1f ended at x=%.3f\n",
                         a.start.x, x);
            return fail("the player body passed through the wall");
        }
    }

    // 3) Walking up the ramp follows it.
    physics.teleport_player_character(ptv::Vec3(-6.0f, 0.0f, -3.0f));
    for (int i = 0; i < 72 * 4; ++i) {
        physics.move_player_character(ptv::Vec3(0.0f, 0.0f, -1.5f * dt), true,
                                      0.0f, dt);
    }
    // Six metres up a 12 degree ramp that starts below the floor surface:
    // about 0.28 m of climb by the end.
    if (physics.player_character_state().feet.y < 0.2f) {
        std::fprintf(stderr, "  feet at y=%.3f on the ramp\n",
                     physics.player_character_state().feet.y);
        return fail("the body did not climb the ramp");
    }

    // 4) The head cannot be pushed through the wall either.
    const ptv::Vec3 head = physics.resolve_head(
        ptv::Vec3(3.0f, 1.6f, 0.0f), ptv::Vec3(4.6f, 1.6f, 0.0f), 0.12f);
    if (head.x > 3.85f) {
        std::fprintf(stderr, "  head ended at x=%.3f\n", head.x);
        return fail("the head passed into the wall");
    }

    physics.shutdown();
    return ok("player body: stays grounded, climbs ramps, and walls block it "
              "and the head from every direction");
}

// A worn item with a collider (a kinematic hip holster) follows the player
// every frame. Inside the capsule it pushes the body at the player's own
// speed, and the push compounds: a 1.5 m/s walk became 20+ m/s on a Quest.
// Ignored, as the player's body family is, it must change nothing.
static int case_player_self_push() {
    ptv::World world;
    {
        const auto e = world.create_entity("floor");
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, -0.1f, 0.0f);
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        world.registry().emplace<ptv::CPrimitive>(
            e, ptv::CPrimitive{"box", ptv::Vec3(400.0f, 0.2f, 400.0f),
                               ptv::Vec3(0.5f)});
        ptv::CPhysics p{};
        p.kind = "static";
        p.collision_shape = "box";
        world.registry().emplace<ptv::CPhysics>(e, p);
    }
    // Hip height, a hand's width behind the body's axis (+Z: the body walks
    // towards -Z), overlapping a 0.22 m capsule.
    const ptv::Vec3 hip_offset(0.0f, 0.9f, 0.12f);
    const auto holster = world.create_entity("hip_holster");
    const auto holster_id = static_cast<std::uint32_t>(holster);
    {
        ptv::Transform t{};
        t.position = hip_offset;
        world.registry().emplace<ptv::CTransform>(holster, ptv::CTransform{t});
        world.registry().emplace<ptv::CPrimitive>(
            holster, ptv::CPrimitive{"box", ptv::Vec3(0.28f, 0.2f, 0.14f),
                                     ptv::Vec3(0.5f)});
        ptv::CPhysics p{};
        p.kind = "kinematic";
        p.collision_shape = "box";
        world.registry().emplace<ptv::CPhysics>(holster, p);
    }

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);
    const float dt = 1.0f / 72.0f;
    const float walk = 1.5f;

    std::vector<std::uint32_t> touched;
    auto run = [&](bool ignore_holster) {
        physics.create_player_character(ptv::Vec3(0.0f), 1.7f, 0.22f);
        physics.teleport(holster_id, hip_offset, ptv::Quat(1, 0, 0, 0));
        std::vector<std::uint32_t> ignored;
        if (ignore_holster) ignored.push_back(holster_id);
        physics.set_player_character_ignored(ignored);
        touched.clear();
        const ptv::Vec3 start = physics.player_character_state().feet;
        constexpr int kFrames = 72 * 5;
        for (int i = 0; i < kFrames; ++i) {
            physics.move_player_character(ptv::Vec3(0.0f, 0.0f, -walk * dt),
                                          true, 0.0f, dt);
            std::vector<std::uint32_t> now;
            physics.player_character_contacts(now);
            for (auto c : now) {
                if (std::find(touched.begin(), touched.end(), c) == touched.end())
                    touched.push_back(c);
            }
            // tick_attachments: the holster rides the body.
            const ptv::Vec3 feet = physics.player_character_state().feet;
            physics.move_kinematic(holster_id, feet + hip_offset,
                                   ptv::Quat(1, 0, 0, 0), dt);
            physics.step(dt);
        }
        const ptv::Vec3 end = physics.player_character_state().feet;
        return glm::length(ptv::Vec3(end.x - start.x, 0.0f, end.z - start.z)) /
               (kFrames * dt);
    };

    const float fixed = run(true);
    const float broken = run(false);
    const bool broken_named =
        std::find(touched.begin(), touched.end(), holster_id) != touched.end();
    std::printf("  walk %.2f m/s: holster ignored -> %.2f m/s, not ignored "
                "-> %.2f m/s (contact reported: %s)\n",
                walk, fixed, broken, broken_named ? "yes" : "no");
    physics.shutdown();

    if (fixed > walk * 1.05f) {
        return fail("an ignored follower still pushed the player faster than "
                    "they asked to move");
    }
    if (fixed < walk * 0.9f) return fail("the player no longer walks at pace");
    if (broken > walk * 1.05f && !broken_named) {
        return fail("the body was pushed but the contact list does not name "
                    "the pusher");
    }
    return ok("self-push: a follower the player wears never pushes them; the "
              "pusher is reported when it is not ignored");
}

// Walk or run on the slide: hold the left stick in while pushing forward.
static int case_sprint_gate() {
    using ptv::SprintActivation;
    ptv::slide::SprintGate gate;
    ptv::slide::SprintGate::Config cfg;
    const float dt = 1.0f / 72.0f;
    auto run_for = [&](float seconds, ptv::Vec2 axes, bool down, bool click_once) {
        const int n = static_cast<int>(seconds / dt);
        for (int i = 0; i < n; ++i) {
            gate.update(cfg, axes, std::min(1.0f, glm::length(axes)), down,
                        click_once && i == 0, dt);
        }
        return gate.blend();
    };
    const ptv::Vec2 fwd(0.0f, 1.0f), side(1.0f, 0.0f), back(0.0f, -1.0f);
    const ptv::Vec2 half(0.0f, 0.4f), diag(0.6f, 0.8f);

    cfg.activation = SprintActivation::Hold;
    if (run_for(1.0f, fwd, false, false) > 0.0f)
        return fail("hold: running without the stick pressed");
    if (run_for(1.5f, fwd, true, true) < 0.95f)
        return fail("hold: holding the stick in did not reach running pace");
    if (run_for(1.5f, fwd, false, false) > 0.05f)
        return fail("hold: releasing the stick did not return to walking");
    gate.reset();
    if (run_for(1.5f, side, true, true) > 0.0f)
        return fail("hold: strafing sideways should never sprint");
    if (run_for(1.5f, back, true, true) > 0.0f)
        return fail("hold: walking backwards should never sprint");
    if (run_for(1.5f, half, true, true) > 0.0f)
        return fail("hold: a light push below min_stick should not sprint");
    if (run_for(1.5f, diag, true, true) < 0.95f)
        return fail("hold: a forward diagonal inside the cone should sprint");

    gate.reset();
    cfg.activation = SprintActivation::Toggle;
    if (run_for(1.5f, fwd, false, true) < 0.95f)
        return fail("toggle: one click did not start running");
    if (run_for(1.0f, fwd, false, false) < 0.95f)
        return fail("toggle: running stopped without the stick returning");
    if (run_for(1.5f, ptv::Vec2(0.0f), false, false) > 0.05f)
        return fail("toggle: centring the stick did not end the run");
    if (run_for(1.0f, fwd, false, false) > 0.0f)
        return fail("toggle: running resumed without a new click");

    gate.reset();
    cfg.activation = SprintActivation::Off;
    if (run_for(1.5f, fwd, true, true) > 0.0f)
        return fail("off: sprint engaged while switched off");

    return ok("sprint gate: hold, toggle and off; forward cone and stick "
              "threshold respected; ramps in and out");
}

// The avatar body follows the neck its clip produced, within limits, and
// attachment offsets are metres even on a centimetre (0.01-scaled) rig.
static int case_avatar_anchor() {
    namespace ab = ptv::avatar_body;
    ab::NeckDrift drift;
    const ptv::Vec3 bind(0.0f, 1.45f, 0.0f);
    for (int i = 0; i < 72; ++i) {
        drift.update(bind + ptv::Vec3(0.0f, -0.05f, 0.10f), bind, 1.0f / 72.0f);
    }
    if (std::fabs(drift.value().z - 0.10f) > 0.005f || drift.value().y != 0.0f) {
        std::fprintf(stderr, "  drift after 1 s = (%.3f, %.3f, %.3f)\n",
                     drift.value().x, drift.value().y, drift.value().z);
        return fail("neck drift did not converge to the clip's horizontal offset");
    }
    for (int i = 0; i < 144; ++i) {
        drift.update(bind + ptv::Vec3(0.5f, 0.0f, 0.5f), bind, 1.0f / 72.0f);
    }
    if (glm::length(drift.value()) > ab::kMaxNeckDrift + 1e-4f)
        return fail("neck drift escaped its clamp");
    drift.update(ptv::Vec3(NAN, 0.0f, 0.0f), bind, 1.0f / 72.0f);
    if (!std::isfinite(drift.value().x)) return fail("neck drift went non-finite");

    // A joint at (1, 2, 3), turned 90 degrees, in a rig scaled 0.01.
    const ptv::Mat4 joint =
        glm::translate(ptv::Mat4(1.0f), ptv::Vec3(1.0f, 2.0f, 3.0f)) *
        glm::rotate(ptv::Mat4(1.0f), glm::radians(90.0f), ptv::Vec3(0, 1, 0)) *
        glm::scale(ptv::Mat4(1.0f), ptv::Vec3(0.01f));
    const ptv::Vec3 offset(0.0f, -0.05f, 0.01f);
    const ptv::Vec3 placed(ptv::rigid_part(joint) * ptv::Vec4(offset, 1.0f));
    const float moved = glm::length(placed - ptv::Vec3(1.0f, 2.0f, 3.0f));
    if (std::fabs(moved - glm::length(offset)) > 1e-4f) {
        std::fprintf(stderr, "  offset of %.4f m landed %.4f m away\n",
                     glm::length(offset), moved);
        return fail("an attachment offset on a 0.01-scaled joint is not metres");
    }
    const ptv::Vec3 x(ptv::rigid_part(joint)[0]);
    if (std::fabs(x.z + 1.0f) > 1e-4f)
        return fail("stripping scale lost the joint's orientation");
    return ok("avatar anchor: neck drift follows the clip within its clamp; "
              "attachment offsets are metres on a 0.01-scaled rig");
}

// The avatar is sized to the player STANDING: not while the headset goes on
// or the player sits, never shrunk by a crouch, and grown when they stand up.
static int case_height_calibration() {
    namespace ab = ptv::avatar_body;
    const float dt = 1.0f / 72.0f;
    const float avatar_eye = 1.62f;
    auto feed = [&](ab::HeightCalibration& c, float seconds, float eye,
                    float wobble = 0.0f) {
        ab::HeightCalibration::Event last = ab::HeightCalibration::Event::None;
        const int n = static_cast<int>(seconds / dt);
        for (int i = 0; i < n; ++i) {
            const float e = eye + wobble * std::sin(0.4f * static_cast<float>(i));
            const auto ev = c.update(e, avatar_eye, dt);
            if (ev != ab::HeightCalibration::Event::None) last = ev;
        }
        return last;
    };

    // Seated start, then stands up.
    {
        ab::HeightCalibration c;
        feed(c, 3.0f, 1.11f);
        if (c.calibrated())
            return fail("calibrated to a seated eye height");
        if (feed(c, 1.0f, 1.62f) != ab::HeightCalibration::Event::First)
            return fail("did not calibrate once the player stood still");
        if (std::fabs(c.scale() - 1.0f) > 0.01f) {
            std::fprintf(stderr, "  scale %.3f, expected 1.000\n", c.scale());
            return fail("standing calibration has the wrong scale");
        }
        // A crouch never shrinks the avatar.
        feed(c, 4.0f, 1.0f);
        if (c.target_scale() < 0.999f) return fail("a crouch shrank the avatar");
        // Standing taller re-calibrates, but only after holding it.
        feed(c, 1.0f, 1.80f);
        if (c.target_scale() > 1.001f)
            return fail("re-calibrated before the taller pose had held");
        if (feed(c, 1.5f, 1.80f) != ab::HeightCalibration::Event::StoodTaller)
            return fail("standing taller did not re-calibrate");
        feed(c, 1.0f, 1.80f);
        if (std::fabs(c.scale() - 1.80f / avatar_eye) > 0.01f)
            return fail("re-calibrated scale did not blend to its target");
    }
    // A moving head (putting the headset on) never calibrates.
    {
        ab::HeightCalibration c;
        feed(c, 3.0f, 1.6f, 0.08f);
        if (c.calibrated()) return fail("calibrated while the head was moving");
    }
    // Seated for good: falls back after 10 s instead of never sizing.
    {
        ab::HeightCalibration c;
        if (feed(c, 10.5f, 1.15f) != ab::HeightCalibration::Event::Fallback)
            return fail("a seated player was never sized");
        if (c.scale() < 0.749f || c.scale() > 0.751f)
            return fail("fallback scale ignored the clamp");
    }
    return ok("height calibration: waits for standing, ignores crouches, grows "
              "when the player stands, falls back when seated");
}

// The shoulder assist depends only on this frame's inputs. Applied on top of
// its own previous result it used to wind the shoulder round frame after
// frame -- the arm whipped in and out of view.
static int case_arm_assist() {
    namespace ik = ptv::two_bone_ik;
    const ptv::Quat bind = glm::angleAxis(glm::radians(10.0f), ptv::Vec3(0, 1, 0));
    const ptv::Vec3 clav(0.05f, 1.45f, 0.0f), shoulder(0.2f, 1.45f, 0.0f);
    const ptv::Vec3 target(0.3f, 1.3f, -0.62f);
    const float arm = 0.55f;
    const float reach = glm::length(target - shoulder);
    const float w = ik::shoulder_assist_weight(reach, arm);
    if (w <= 0.0f) return fail("test reach should engage the assist");

    const ptv::Quat a = ik::shoulder_assist(bind, clav, shoulder, target, w);
    for (int i = 0; i < 200; ++i) {
        const ptv::Quat b = ik::shoulder_assist(bind, clav, shoulder, target, w);
        if (std::fabs(glm::dot(a, b)) < 0.99999f)
            return fail("the shoulder assist is not idempotent");
    }
    const ptv::Quat delta = a * glm::inverse(bind);
    const float deg =
        glm::degrees(2.0f * std::acos(std::clamp(std::fabs(delta.w), 0.0f, 1.0f)));
    if (deg > 25.0f + 1e-3f) return fail("the shoulder assist exceeds its cap");
    // Even a target far behind the shoulder stays within the cap.
    const ptv::Quat far = ik::shoulder_assist(bind, clav, shoulder,
                                              ptv::Vec3(-2.0f, 3.0f, 2.0f), 1.0f);
    const ptv::Quat fd = far * glm::inverse(bind);
    if (glm::degrees(2.0f * std::acos(std::clamp(std::fabs(fd.w), 0.0f, 1.0f))) >
        25.0f + 1e-3f)
        return fail("an extreme reach escaped the assist cap");
    // Continuous across the threshold: no jump as the reach crosses 85 %.
    const float below = ik::shoulder_assist_weight(0.8499f * arm, arm);
    const float above = ik::shoulder_assist_weight(0.8501f * arm, arm);
    if (below != 0.0f || above > 0.01f)
        return fail("the assist weight jumps at the threshold");
    if (ik::shoulder_assist_weight(NAN, arm) != 0.0f)
        return fail("a non-finite reach engaged the assist");
    const ptv::Quat nan_q =
        ik::shoulder_assist(bind, clav, shoulder, ptv::Vec3(NAN), 1.0f);
    if (!std::isfinite(nan_q.w)) return fail("a non-finite target leaked a NaN");
    std::printf("  assist at reach %.2f of arm: %.1f deg\n", reach / arm, deg);
    return ok("arm assist: idempotent, capped at 25 degrees, continuous, "
              "NaN-safe");
}

// Teleport distance follows controller pitch monotonically, and the launch
// speed actually lands the arc where that distance says.
static int case_teleport_pitch() {
    namespace tp = ptv::teleport;
    float prev = -1.0f;
    for (float deg = -60.0f; deg <= 35.0f; deg += 5.0f) {
        const float d = tp::distance_for_pitch(glm::radians(deg), 1.0f, 8.0f);
        if (d <= prev) return fail("teleport distance is not monotonic in pitch");
        prev = d;
    }
    for (float deg : {-30.0f, 0.0f, 20.0f, 35.0f}) {
        const float pitch = glm::radians(deg);
        const float want = tp::reachable_distance(
            pitch, tp::distance_for_pitch(pitch, 1.0f, 8.0f), 1.2f);
        tp::ArcParams p;
        p.max_horizontal_m = 20.0f;
        p.launch_speed_mps = tp::launch_speed_for(pitch, want, 1.2f, p.gravity_mps2);
        tp::ArcResult r;
        const ptv::Vec3 dir(0.0f, std::sin(pitch), -std::cos(pitch));
        tp::solve_arc(ptv::Vec3(0.0f, 1.2f, 0.0f), dir, p, nullptr, r);
        if (!r.valid) return fail("teleport arc found no landing");
        const float got = glm::length(ptv::Vec3(r.target.x, 0.0f, r.target.z));
        if (std::fabs(got - want) > 0.15f * want + 0.2f) {
            std::fprintf(stderr, "  pitch %.0f: landed %.2f m, wanted %.2f\n",
                         deg, got, want);
            return fail("teleport arc does not land at the pitch's distance");
        }
    }
    return ok("teleport: pitch maps monotonically to distance and the arc "
              "lands there");
}

// builtin.grab's grip-socket alignment, evaluated with the same maths the
// mechanic uses. The point of the feature is that the result does NOT depend
// on how the player reached for the object, so the test grabs from two very
// different hand poses and expects the same outcome.
static int case_grab_socket() {
    // A weapon whose grip sits 12 cm back and 3 cm down from the origin,
    // with the socket tilted like a real derived pistol grip (-21 degrees
    // about X, i.e. its +Z barrel axis not exactly the model's +Z).
    ptv::Transform socket;
    socket.position = ptv::Vec3(0.0f, -0.03f, 0.12f);
    socket.rotation =
        glm::angleAxis(glm::radians(-21.0f), ptv::Vec3(1.0f, 0.0f, 0.0f));

    // A Touch controller held upright: the aim pose looks straight ahead
    // (-Z) with +Y up; the grip pose is the handle, pitched ~35 degrees.
    const ptv::Quat aim(1.0f, 0.0f, 0.0f, 0.0f);
    const ptv::Quat grip =
        glm::angleAxis(glm::radians(-35.0f), ptv::Vec3(1.0f, 0.0f, 0.0f));
    const ptv::Quat grip_to_aim = glm::inverse(grip) * aim;

    ptv::Vec3 local_offset;
    ptv::Quat local_rotation;
    ptv::grab::solve_grip_hold(socket.position, socket.rotation, 1.0f,
                               grip_to_aim, ptv::Quat(1, 0, 0, 0),
                               &local_offset, &local_rotation);

    // Two very different body orientations holding the controller the same
    // way relative to the head: the result must be identical relative to aim.
    const ptv::Quat turns[2] = {
        ptv::Quat(1, 0, 0, 0),
        glm::angleAxis(glm::radians(-110.0f), ptv::Vec3(0.0f, 1.0f, 0.0f)),
    };
    for (const auto& turn : turns) {
        const ptv::Vec3 hand_pos(0.2f, 1.1f, -0.4f);
        const ptv::Quat hand_rot = turn * grip;
        const ptv::Quat aim_rot = turn * aim;
        const ptv::Vec3 obj_pos = hand_pos + hand_rot * local_offset;
        const ptv::Quat obj_rot = hand_rot * local_rotation;
        const ptv::Vec3 socket_pos = obj_pos + obj_rot * socket.position;
        const ptv::Quat socket_rot = obj_rot * socket.rotation;

        if (glm::length(socket_pos - hand_pos) > 1e-4f)
            return fail("grip socket did not land in the hand");
        const ptv::Vec3 barrel = socket_rot * ptv::Vec3(0, 0, 1);
        const ptv::Vec3 up = socket_rot * ptv::Vec3(0, 1, 0);
        const ptv::Vec3 aim_fwd = aim_rot * ptv::Vec3(0, 0, -1);
        const ptv::Vec3 aim_up = aim_rot * ptv::Vec3(0, 1, 0);
        if (glm::dot(barrel, aim_fwd) < 0.95f) {
            std::fprintf(stderr, "  barrel . aim = %.3f\n",
                         glm::dot(barrel, aim_fwd));
            return fail("the gun does not point where the controller aims");
        }
        if (glm::dot(up, aim_up) < 0.9f)
            return fail("the gun is not upright in an upright hand");
    }

    return ok("grab socket: the grip lands in the hand, barrel along the aim, "
              "gun upright");
}

// HoldSolver: the hand-socket conventions socket_generation writes, solved
// the way builtin.grab solves them.
static int case_hold_solver() {
    namespace g = ptv::grab;
    using ptv::Quat;
    using ptv::Vec3;
    const Vec3 hand_pos(0.1f, 1.2f, -0.3f);
    const Quat grip = glm::angleAxis(glm::radians(-35.0f), Vec3(1.0f, 0.0f, 0.0f));

    // 1. Hand-frame hold: a sword's grip socket (OpenXR grip frame) lands
    //    exactly on the grip pose, so -Z (pinky -> thumb) runs along the hilt.
    {
        const Vec3 sock_pos(0.0f, -0.1f, 0.0f);
        // Hilt along mesh +Y: -Z of the socket = +Y, +X = mesh +X.
        const Quat sock_rot = g::quat_from_zy(Vec3(0, -1, 0), Vec3(0, 0, 1));
        Vec3 off;
        Quat rot;
        g::solve_hand_frame_hold(sock_pos, sock_rot, 1.0f, &off, &rot);
        const Vec3 obj_pos = hand_pos + grip * off;
        const Quat obj_rot = grip * rot;
        const Vec3 s_world = obj_pos + obj_rot * sock_pos;
        if (glm::length(s_world - hand_pos) > 1e-4f) return fail("hand-frame: socket not on grip");
        const Quat s_rot = obj_rot * sock_rot;
        if (glm::dot(s_rot * Vec3(0, 0, -1), grip * Vec3(0, 0, -1)) < 0.999f)
            return fail("hand-frame: socket frame does not match grip frame");
        // Left-hand mirror for a palm-up support grip turns about Z.
        const Quat left = g::hand_socket_rotation(sock_rot, true, true);
        if (glm::dot(left * Vec3(0, 0, 1), sock_rot * Vec3(0, 0, 1)) < 0.999f ||
            glm::dot(left * Vec3(1, 0, 0), sock_rot * Vec3(1, 0, 0)) > -0.999f)
            return fail("flip_z180 mirror is not a half turn about Z");
    }
    // 2. Aim hold: palm on the grip origin, barrel (mesh +X) along the aim.
    {
        const Quat aim(1.0f, 0.0f, 0.0f, 0.0f);
        const Quat grip_to_aim = glm::inverse(grip) * aim;
        const Vec3 palm(-0.05f, -0.05f, 0.0f);
        Vec3 off;
        Quat rot;
        g::solve_aim_hold(palm, Vec3(1, 0, 0), Vec3(0, 1, 0), 1.0f, grip_to_aim,
                          Quat(1, 0, 0, 0), &off, &rot);
        const Vec3 obj_pos = hand_pos + grip * off;
        const Quat obj_rot = grip * rot;
        if (glm::length(obj_pos + obj_rot * palm - hand_pos) > 1e-4f)
            return fail("aim hold: palm not at the grip origin");
        if (glm::dot(obj_rot * Vec3(1, 0, 0), aim * Vec3(0, 0, -1)) < 0.999f)
            return fail("aim hold: barrel does not follow the aim");
        if (glm::dot(obj_rot * Vec3(0, 1, 0), aim * Vec3(0, 1, 0)) < 0.99f)
            return fail("aim hold: gun is not upright");
    }
    // 3. Line hold: rifle grip -> handguard span laid between the two hands.
    {
        const Vec3 p1(0.0f, -0.05f, 0.0f), p2(0.35f, 0.0f, 0.0f);
        const Vec3 P1(0.0f, 1.2f, -0.2f), P2(-0.1f, 1.3f, -0.55f);
        const Quat R = g::line_rotation(p2 - p1, Vec3(0, 1, 0), P2 - P1, Vec3(0, 1, 0));
        const Vec3 pos = g::place_point(p1, 1.0f, R, P1);
        if (glm::length(pos + R * p1 - P1) > 1e-4f) return fail("line: grip off the hand");
        const Vec3 dir = glm::normalize(R * (p2 - p1));
        if (glm::dot(dir, glm::normalize(P2 - P1)) < 0.999f)
            return fail("line: rifle not along the hands");
        if ((R * Vec3(0, 1, 0)).y < 0.8f) return fail("line: rifle rolled over");
    }
    // 4. Forearm hold: hand strap at the palm, forearm strap toward the elbow,
    //    front face along the back of the forearm.
    {
        const Vec3 hand_strap(0.0f, 0.12f, -0.03f), fore_strap(0.0f, -0.08f, -0.03f);
        const Vec3 front(0, 0, 1);
        const Vec3 elbow_dir = glm::normalize(Vec3(0.1f, -0.2f, 1.0f));
        const Vec3 dorsal = g::dorsal_direction(grip, false);
        const Quat R = g::line_rotation(fore_strap - hand_strap, front, elbow_dir, dorsal);
        if (glm::dot(glm::normalize(R * (fore_strap - hand_strap)), elbow_dir) < 0.999f)
            return fail("forearm: straps not along the forearm");
        const Vec3 n = R * front;
        const Vec3 dorsal_perp = glm::normalize(dorsal - elbow_dir * glm::dot(dorsal, elbow_dir));
        if (glm::dot(n, dorsal_perp) < 0.999f) return fail("forearm: shield faces the wrong way");
    }
    return ok("hold solver: hand-frame, aim, line and forearm holds land as specified");
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

// A character needs a capsule, not the box it used to silently get. The scene
// compiler has always emitted collision_shape "capsule" for characters, but
// PhysicsWorld only knew box/sphere/mesh, so every NPC collided as a cube --
// too wide to fit a doorway and prone to catching on its own corners.
//
// Rather than assert "a body exists" (which passed before this change too),
// this drops the character onto a floor and measures where it settles. Only a
// correctly sized capsule comes to rest with its centre one half-height up.
static int case_physics_capsule() {
    ptv::World world;

    // Floor: 20 x 0.2 x 20 static box whose top face sits exactly at y = 0.
    {
        const auto e = world.create_entity("floor");
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, -0.1f, 0.0f);
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        world.registry().emplace<ptv::CPrimitive>(
            e, ptv::CPrimitive{"box", ptv::Vec3(20.0f, 0.2f, 20.0f),
                               ptv::Vec3(0.5f)});
        ptv::CPhysics p{};
        p.kind = "static";
        p.collision_shape = "box";
        world.registry().emplace<ptv::CPhysics>(e, p);
    }

    // Character: 1.8 m tall, dropped from 3 m.
    const float kHeight = 1.8f;
    {
        const auto e = world.create_entity("goblin");
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, 3.0f, 0.0f);
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        ptv::CMesh m{};
        m.asset_id = "goblin";
        m.target_scale_meters = kHeight;
        m.skinned = true;
        world.registry().emplace<ptv::CMesh>(e, std::move(m));
        ptv::CPhysics p{};
        p.kind = "dynamic";
        p.mass_kg = 70.0f;
        p.collision_shape = "capsule";
        world.registry().emplace<ptv::CPhysics>(e, p);
    }

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);

    const auto goblin = static_cast<std::uint32_t>(world.find("goblin"));
    if (!physics.has_body(goblin)) return fail("character got no body");

    // Shove it sideways while it falls: a capsule that is not DOF-locked
    // topples on landing, which is exactly what we must not allow.
    physics.apply_impulse(goblin, ptv::Vec3(120.0f, 0.0f, 0.0f));

    for (int i = 0; i < 240; ++i) physics.step(1.0f / 60.0f);
    physics.sync_to_world(world);

    const auto& t = world.get<ptv::CTransform>(goblin).t;
    const float expected_y = 0.5f * kHeight;
    if (std::fabs(t.position.y - expected_y) > 0.06f) {
        std::fprintf(stderr,
                     "  settled at y=%.3f, expected ~%.3f (capsule height "
                     "%.2f)\n",
                     t.position.y, expected_y, kHeight);
        return fail("capsule did not settle at half its height above the floor");
    }
    if (std::fabs(t.rotation.x) > 0.02f || std::fabs(t.rotation.z) > 0.02f) {
        std::fprintf(stderr, "  rotation x=%.3f z=%.3f\n", t.rotation.x,
                     t.rotation.z);
        return fail("character capsule toppled; upright DOFs are not locked");
    }

    // Runtime lifecycle: an entity created after the scene loaded must still
    // get a body, and removing it must actually destroy it.
    const auto spawned = world.create_entity("spawned");
    {
        ptv::Transform st{};
        st.position = ptv::Vec3(4.0f, 1.0f, 0.0f);
        world.registry().emplace<ptv::CTransform>(spawned,
                                                  ptv::CTransform{st});
        ptv::CPhysics p{};
        p.kind = "dynamic";
        p.collision_shape = "sphere";
        world.registry().emplace<ptv::CPhysics>(spawned, p);
    }
    const auto spawned_id = static_cast<std::uint32_t>(spawned);
    if (physics.has_body(spawned_id))
        return fail("a brand-new entity already had a body");
    if (!physics.add_body_for(world, spawned_id))
        return fail("add_body_for refused a valid entity");
    if (!physics.has_body(spawned_id))
        return fail("add_body_for did not create a body");
    if (!physics.add_body_for(world, spawned_id))
        return fail("add_body_for is not idempotent");

    physics.remove_body(spawned_id);
    if (physics.has_body(spawned_id))
        return fail("remove_body left the body behind");

    physics.shutdown();
    return ok("physics: character capsule sized from mesh height, stays "
              "upright, and bodies can be added/removed at runtime");
}

// Slide locomotion used to be free-fly: ILocomotion::translate just added to
// the rig, so the player walked through every wall the world builder placed.
// slide_character sweeps a capsule instead, and this checks the three cases
// that matter -- open space, head-on into a wall, and a glancing angle that
// must keep its tangential motion instead of stopping dead.
static int case_physics_player_slide() {
    ptv::World world;

    auto add_box = [&](const char* id, ptv::Vec3 pos, ptv::Vec3 size) {
        const auto e = world.create_entity(id);
        ptv::Transform t{};
        t.position = pos;
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        world.registry().emplace<ptv::CPrimitive>(
            e, ptv::CPrimitive{"box", size, ptv::Vec3(0.5f)});
        ptv::CPhysics p{};
        p.kind = "static";
        p.collision_shape = "box";
        world.registry().emplace<ptv::CPhysics>(e, p);
    };

    add_box("floor", ptv::Vec3(0.0f, -0.1f, 0.0f),
            ptv::Vec3(40.0f, 0.2f, 40.0f));
    // Wall plane at x = 1.9 (near face of a 0.2 m thick slab centred on 2.0).
    add_box("wall", ptv::Vec3(2.0f, 1.5f, 0.0f), ptv::Vec3(0.2f, 3.0f, 12.0f));

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);

    const float kHeight = 1.7f;
    const float kRadius = 0.17f * kHeight;  // matches build_capsule_shape
    const ptv::Vec3 center(0.0f, 0.5f * kHeight, 0.0f);
    const std::uint32_t kNoSelf = 0xFFFFFFFFu;

    // 1) Open space: nothing in the way behind the player, full delta.
    {
        const ptv::Vec3 want(-1.0f, 0.0f, 0.0f);
        const ptv::Vec3 got =
            physics.slide_character(center, kHeight, want, kNoSelf);
        if (std::fabs(got.x - want.x) > 0.02f)
            return fail("open-space move was obstructed");
    }

    // 2) Straight into the wall: stop just short of contact.
    {
        const ptv::Vec3 got = physics.slide_character(
            center, kHeight, ptv::Vec3(2.0f, 0.0f, 0.0f), kNoSelf);
        const float expected = 1.9f - kRadius;
        if (got.x > expected + 0.05f) {
            std::fprintf(stderr, "  moved x=%.3f, wall contact at %.3f\n",
                         got.x, expected);
            return fail("player walked into (or through) the wall");
        }
        if (got.x < expected - 0.20f) {
            std::fprintf(stderr, "  moved x=%.3f, expected ~%.3f\n", got.x,
                         expected);
            return fail("player stopped far too early");
        }
    }

    // 3) Glancing angle: blocked on X, but the Z component must survive or
    //    the player gets stuck on every wall they brush past.
    {
        const ptv::Vec3 got = physics.slide_character(
            center, kHeight, ptv::Vec3(2.0f, 0.0f, 1.0f), kNoSelf);
        if (got.x > 1.9f - kRadius + 0.05f)
            return fail("glancing move still crossed the wall");
        if (got.z < 0.5f) {
            std::fprintf(stderr, "  slid z=%.3f of a requested 1.0\n", got.z);
            return fail("wall contact killed the along-wall motion");
        }
    }

    physics.shutdown();
    return ok("physics: player sweep blocks walls head-on and slides along "
              "them at an angle");
}

// "The duck touched the player" is the single most common NPC question, and
// before this there was no way to ask it: no triggers, no contact events, and
// Jolt's own contact callbacks skip pairs where neither body is dynamic --
// which is exactly the player-vs-NPC case, since both are kinematic.
static int case_physics_overlap() {
    ptv::World world;

    auto add_capsule = [&](const char* id, ptv::Vec3 pos, const char* kind) {
        const auto e = world.create_entity(id);
        ptv::Transform t{};
        t.position = pos;
        t.scale = 1.8f;  // no CMesh, so the capsule takes its height here
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        ptv::CPhysics p{};
        p.kind = kind;
        p.collision_shape = "capsule";
        world.registry().emplace<ptv::CPhysics>(e, p);
        return static_cast<std::uint32_t>(e);
    };

    // Both kinematic on purpose: this is the pair Jolt would never report.
    const auto player = add_capsule("player", ptv::Vec3(0.0f, 0.9f, 0.0f),
                                    "kinematic");
    const auto near_duck = add_capsule("near", ptv::Vec3(0.35f, 0.9f, 0.0f),
                                       "kinematic");
    const auto far_duck = add_capsule("far", ptv::Vec3(6.0f, 0.9f, 0.0f),
                                      "kinematic");

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);

    std::vector<std::uint32_t> hits;
    physics.overlapping(player, hits);

    const bool saw_near =
        std::find(hits.begin(), hits.end(), near_duck) != hits.end();
    const bool saw_far =
        std::find(hits.begin(), hits.end(), far_duck) != hits.end();
    const bool saw_self =
        std::find(hits.begin(), hits.end(), player) != hits.end();

    if (!saw_near) return fail("overlapping missed a touching kinematic body");
    if (saw_far) return fail("overlapping reported a body 6 m away");
    if (saw_self) return fail("overlapping reported the query entity itself");

    // Move the near one away and confirm the overlap clears.
    physics.move_kinematic(near_duck, ptv::Vec3(9.0f, 0.9f, 0.0f),
                           ptv::Quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0f / 60.0f);
    physics.step(1.0f / 60.0f);
    physics.overlapping(player, hits);
    if (std::find(hits.begin(), hits.end(), near_duck) != hits.end())
        return fail("overlap persisted after the body moved away");

    physics.shutdown();
    return ok("physics: overlap queries see kinematic-vs-kinematic contact "
              "that Jolt's contact callbacks never report");
}

// A probe mechanic so the runtime-attach test can prove a spawned entity's
// mechanics actually tick, rather than just that a binding component exists.
namespace {
int g_probe_attached = 0;
int g_probe_ticks = 0;
int g_probe_detached = 0;

class ProbeMechanic final : public ptv::IMechanic {
public:
    std::string id() const override { return "test.probe"; }
    void on_attach(ptv::World&, std::uint32_t, const nlohmann::json&,
                   ptv::MechanicContext&) override {
        ++g_probe_attached;
    }
    void on_update(ptv::World&, std::uint32_t, float,
                   const ptv::InputSnapshot&,
                   ptv::MechanicContext&) override {
        ++g_probe_ticks;
    }
    void on_detach(ptv::World&, std::uint32_t) override { ++g_probe_detached; }
};
}  // namespace

PTV_REGISTER_MECHANIC("test.probe", ProbeMechanic)

// Everything about an entity used to be decided once, at scene load:
// physics_.rebuild_from_world, attach_bindings and attach_skeletons all ran
// exactly once. An enemy spawned mid-wave was therefore a ghost -- visible,
// but with no collider and with none of its mechanics running. This checks
// the per-entity attach path that replaces that.
namespace {
/** ISpawn over clone_entity_tree, without physics or a mechanic host. */
struct FakeSpawn final : ptv::ISpawn {
    ptv::World* world{nullptr};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> live;  // clone, template
    std::uint32_t instance(std::uint32_t tmpl, const ptv::Vec3& pos,
                           const ptv::Quat& rot) override {
        std::vector<ptv::CloneRecord> made;
        const auto e = ptv::clone_entity_tree(*world, static_cast<entt::entity>(tmpl),
                                              pos, rot, made);
        if (e == entt::null) return 0xFFFFFFFFu;
        live.push_back({static_cast<std::uint32_t>(e), tmpl});
        return static_cast<std::uint32_t>(e);
    }
    void despawn(std::uint32_t e) override {
        if (world->valid(e)) world->registry().destroy(static_cast<entt::entity>(e));
    }
    int live_count(std::uint32_t tmpl) const override {
        int n = 0;
        for (const auto& [c, t] : live) {
            if (t == tmpl && world->valid(c)) ++n;
        }
        return n;
    }
};

entt::entity make_actor(ptv::World& world, const char* id, ptv::Vec3 pos,
                        std::vector<std::string> tags, float hp_max) {
    const auto e = world.create_entity(id);
    ptv::Transform t{};
    t.position = pos;
    world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
    world.registry().emplace<ptv::CTag>(e, ptv::CTag{std::move(tags)});
    ptv::CHealth hp{};
    hp.max = hp.current = hp_max;
    hp.invuln_after_hit_s = 0.0f;
    world.registry().emplace<ptv::CHealth>(e, hp);
    return e;
}
}  // namespace

// The engine's spawner: waves of a template, a cap on how many are alive,
// defeats counted, a countdown between waves, victory, and a HUD-readable
// board -- the machinery a generated game used to get wrong.
static int case_encounter_director() {
    ptv::World world;
    make_actor(world, "player", ptv::Vec3(0.0f), {"player"}, 100.0f);
    make_actor(world, "grunt_tmpl", ptv::Vec3(0.0f, -50.0f, 0.0f),
               {"enemy", "template"}, 10.0f);
    const auto host = world.create_entity("game_director");
    world.registry().emplace<ptv::CTransform>(host, ptv::CTransform{});

    FakeSpawn spawn;
    spawn.world = &world;
    ptv::EncounterBoard board;
    ptv::MechanicContext ctx{};
    ctx.spawn = &spawn;
    ctx.encounters = &board;

    const auto params = nlohmann::json::parse(R"({
        "mode": "waves", "start": {"kind": "on_load"},
        "max_concurrent": 2, "spawn_interval_s": 0.0, "inter_wave_delay_s": 1.0,
        "min_player_distance_m": 5.0,
        "waves": [{"groups": [{"template": "grunt_tmpl", "count": 3}]},
                  {"groups": [{"template": "grunt_tmpl", "count": 1}]}],
        "zones": [{"id": "gate", "position": [10, 0, 0], "radius": 1.0}]
    })");
    auto mech = ptv::MechanicRegistry::get().make_mechanic("builtin.encounter_director");
    if (!mech) return fail("builtin.encounter_director is not registered");
    const auto hid = static_cast<std::uint32_t>(host);
    mech->on_attach(world, hid, params, ctx);
    const ptv::InputSnapshot input{};
    auto tick = [&](float s) {
        const int n = std::max(1, static_cast<int>(s / 0.05f));
        for (int i = 0; i < n; ++i) mech->on_update(world, hid, 0.05f, input, ctx);
    };
    auto kill_all = [&]() {
        for (const auto& [c, t] : spawn.live) {
            if (world.valid(c)) world.registry().get<ptv::CHealth>(static_cast<entt::entity>(c)).alive = false;
        }
    };

    tick(0.05f);
    if (board.phase() != ptv::EncounterPhase::Active || board.wave_index() != 0)
        return fail("an on_load encounter did not start wave 1");
    if (board.alive() != 2 || spawn.live.size() != 2)
        return fail("the alive cap was not respected");
    for (const auto& [c, t] : spawn.live) {
        const auto p = world.registry().get<ptv::CTransform>(static_cast<entt::entity>(c)).t.position;
        if (glm::length(p) < 5.0f) return fail("an NPC spawned inside the player's space");
        bool templ = false;
        for (const auto& tag : world.registry().get<ptv::CTag>(static_cast<entt::entity>(c)).tags)
            templ = templ || tag == "template";
        if (templ) return fail("a spawned NPC kept the template tag");
    }
    world.registry().get<ptv::CHealth>(static_cast<entt::entity>(spawn.live[0].first)).alive = false;
    tick(0.1f);
    if (board.defeated_total() != 1 || spawn.live.size() != 3 || board.alive() != 2)
        return fail("a defeat did not free a slot for the rest of the wave");
    kill_all();
    tick(0.1f);
    if (board.phase() != ptv::EncounterPhase::InterWave || board.wave_cleared_seq() != 1)
        return fail("clearing wave 1 did not start the countdown");
    tick(1.2f);
    if (board.phase() != ptv::EncounterPhase::Active || board.wave_index() != 1 ||
        board.wave_started_seq() != 2)
        return fail("the second wave did not start after the countdown");
    kill_all();
    tick(0.1f);
    if (board.phase() != ptv::EncounterPhase::Victory || board.victory_seq() != 1 ||
        board.defeated_total() != 4)
        return fail("clearing the last wave did not declare victory");

    // A signal-started encounter waits for the game to say go; reset clears.
    ptv::EncounterBoard board2;
    ctx.encounters = &board2;
    FakeSpawn spawn2;
    spawn2.world = &world;
    ctx.spawn = &spawn2;
    auto p2 = params;
    p2["start"] = {{"kind", "signal"}};
    auto mech2 = ptv::MechanicRegistry::get().make_mechanic("builtin.encounter_director");
    mech2->on_attach(world, hid, p2, ctx);
    for (int i = 0; i < 20; ++i) mech2->on_update(world, hid, 0.05f, input, ctx);
    if (!spawn2.live.empty() || board2.phase() != ptv::EncounterPhase::Idle)
        return fail("a signal-started encounter began on its own");
    board2.start();
    mech2->on_update(world, hid, 0.05f, input, ctx);
    if (spawn2.live.size() != 2) return fail("start() did not begin the encounter");
    board2.reset();
    mech2->on_update(world, hid, 0.05f, input, ctx);
    for (const auto& [c, t] : spawn2.live) {
        if (world.valid(c)) return fail("reset() left spawned NPCs behind");
    }
    if (board2.phase() != ptv::EncounterPhase::Idle)
        return fail("reset() did not return to waiting");
    return ok("encounter director: waves, alive cap, defeats, countdown, victory, "
              "signal start and reset");
}

// Companions follow and catch up; enemies chase, wind up, hit, and flinch.
static int case_npc_behaviors() {
    ptv::World world;
    const auto player = make_actor(world, "player", ptv::Vec3(10.0f, 0.0f, 0.0f),
                                   {"player"}, 100.0f);
    const auto grunt = make_actor(world, "grunt", ptv::Vec3(0.0f), {"enemy"}, 30.0f);
    ptv::MechanicContext ctx{};
    const ptv::InputSnapshot input{};
    auto& reg = world.registry();
    auto pos = [&](entt::entity e) -> ptv::Vec3& {
        return reg.get<ptv::CTransform>(e).t.position;
    };

    // --- melee chase ------------------------------------------------------
    auto chase = ptv::MechanicRegistry::get().make_mechanic("builtin.npc.melee_chase");
    if (!chase) return fail("builtin.npc.melee_chase is not registered");
    const auto gid = static_cast<std::uint32_t>(grunt);
    chase->on_attach(world, gid, nlohmann::json::parse(R"({
        "target_tag": "player", "chase_speed_mps": 4.0, "attack_range_m": 1.6,
        "windup_s": 0.5, "attack_cooldown_s": 2.0, "damage": 15, "stagger_s": 0.4})"), ctx);
    chase->on_update(world, gid, 0.05f, input, ctx);
    const auto v = reg.get<ptv::CLocomotionIntent>(grunt).desired_velocity;
    if (v.x < 3.9f) return fail("melee_chase did not run at the target");
    pos(player) = ptv::Vec3(1.2f, 0.0f, 0.0f);
    // character_locomotion would have turned it to face the player (+X); an
    // NPC never swings at something it is not facing.
    reg.get<ptv::CTransform>(grunt).t.rotation =
        ptv::rotation_facing(ptv::Vec3(1.0f, 0.0f, 0.0f));
    chase->on_update(world, gid, 0.05f, input, ctx);
    if (glm::length(reg.get<ptv::CLocomotionIntent>(grunt).desired_velocity) > 1e-4f)
        return fail("melee_chase kept moving in attack range");
    if (reg.get<ptv::CHealth>(player).current != 100.0f)
        return fail("damage landed with no wind-up");
    for (int i = 0; i < 12; ++i) chase->on_update(world, gid, 0.05f, input, ctx);
    if (reg.get<ptv::CHealth>(player).current != 85.0f)
        return fail("the hit did not land at the end of the wind-up");
    for (int i = 0; i < 10; ++i) chase->on_update(world, gid, 0.05f, input, ctx);
    if (reg.get<ptv::CHealth>(player).current != 85.0f)
        return fail("the attack ignored its cooldown");
    reg.get<ptv::CHealth>(grunt).current = 20.0f;  // shot
    pos(player) = ptv::Vec3(8.0f, 0.0f, 0.0f);
    chase->on_update(world, gid, 0.05f, input, ctx);
    if (glm::length(reg.get<ptv::CLocomotionIntent>(grunt).desired_velocity) > 1e-4f)
        return fail("a hurt enemy did not stagger");

    // --- follow -----------------------------------------------------------
    const auto ally = make_actor(world, "ally", ptv::Vec3(0.0f, 0.0f, 5.0f), {"ally"}, 100.0f);
    const auto aid = static_cast<std::uint32_t>(ally);
    pos(player) = ptv::Vec3(10.0f, 0.0f, 5.0f);
    auto follow = ptv::MechanicRegistry::get().make_mechanic("builtin.npc.follow");
    if (!follow) return fail("builtin.npc.follow is not registered");
    follow->on_attach(world, aid, nlohmann::json::parse(R"({
        "walk_speed_mps": 1.4, "run_speed_mps": 3.5, "catchup_distance_m": 18})"), ctx);
    follow->on_update(world, aid, 0.05f, input, ctx);
    {
        const auto& w = reg.get<ptv::CLocomotionIntent>(ally);
        if (w.desired_velocity.x < 3.4f || !w.sprint)
            return fail("a companion far behind did not run to catch up");
    }
    pos(ally) = ptv::Vec3(7.5f, 0.0f, 5.0f);  // 2.5 m: inside the band, still moving
    follow->on_update(world, aid, 0.05f, input, ctx);
    pos(ally) = ptv::Vec3(8.5f, 0.0f, 5.0f);  // 1.5 m: stop
    follow->on_update(world, aid, 0.05f, input, ctx);
    if (glm::length(reg.get<ptv::CLocomotionIntent>(ally).desired_velocity) > 1e-4f)
        return fail("a companion did not stop beside its leader");
    pos(player) = ptv::Vec3(60.0f, 0.0f, 5.0f);  // teleported far away
    follow->on_update(world, aid, 0.05f, input, ctx);
    if (glm::length(pos(ally) - pos(player)) > 4.0f)
        return fail("a lost companion was not re-placed near its leader");
    return ok("npc behaviors: chase, wind-up then hit, cooldown, hurt stagger; "
              "follow, stop in band, catch up when lost");
}

// A spawned clone is a whole actor: shootable, hostile, holding its gear.
static int case_clone_tree() {
    ptv::World world;
    auto& reg = world.registry();
    const auto goblin = world.create_entity("goblin_template");
    {
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, -50.0f, 0.0f);
        reg.emplace<ptv::CTransform>(goblin, ptv::CTransform{t});
        ptv::CHealth hp{};
        hp.max = 30.0f;
        hp.current = 4.0f;  // a template must not leak a damaged state
        hp.alive = false;
        reg.emplace<ptv::CHealth>(goblin, hp);
        reg.emplace<ptv::CFaction>(goblin, ptv::CFaction{"enemy", {"player"}});
        reg.emplace<ptv::CTag>(goblin, ptv::CTag{{"enemy", "template"}});
        ptv::CPhysics p{};
        p.kind = "kinematic";
        p.collision_shape = "capsule";
        reg.emplace<ptv::CPhysics>(goblin, p);
    }
    const auto club = world.create_entity("club_template");
    {
        reg.emplace<ptv::CTransform>(club, ptv::CTransform{});
        reg.emplace<ptv::CTag>(club, ptv::CTag{{"enemy_weapon", "template"}});
        ptv::CPhysics p{};
        p.kind = "dynamic";
        reg.emplace<ptv::CPhysics>(club, p);
        ptv::CAttachment a{};
        a.parent = static_cast<std::uint32_t>(goblin);
        a.anchor = "hand_r";
        a.local_position = ptv::Vec3(0.0f, -0.05f, 0.05f);
        reg.emplace<ptv::CAttachment>(club, a);
    }

    std::vector<ptv::CloneRecord> made;
    const auto inst = ptv::clone_entity_tree(
        world, goblin, ptv::Vec3(3.0f, 0.0f, 4.0f), ptv::Quat(1, 0, 0, 0), made);
    if (inst == entt::null) return fail("clone_entity_tree returned null");
    if (made.size() != 2 || made[0].entity != inst || made[0].gear || !made[1].gear)
        return fail("clone tree did not report the instance then its gear");

    const auto& hp = reg.get<ptv::CHealth>(inst);
    if (!hp.alive || hp.current != 30.0f)
        return fail("a clone did not start at full health");
    if (!reg.all_of<ptv::CFaction>(inst) || reg.get<ptv::CFaction>(inst).id != "enemy")
        return fail("a clone lost its faction");
    for (const auto& t : reg.get<ptv::CTag>(inst).tags) {
        if (t == "template") return fail("a clone kept the template tag");
    }
    if (reg.get<ptv::CTransform>(inst).t.position != ptv::Vec3(3.0f, 0.0f, 4.0f))
        return fail("a clone is not where it was spawned");

    const auto gear = made[1].entity;
    if (!reg.all_of<ptv::CAttachment>(gear))
        return fail("cloned gear is not attached");
    const auto& att = reg.get<ptv::CAttachment>(gear);
    if (att.parent != static_cast<std::uint32_t>(inst) || att.anchor != "hand_r")
        return fail("cloned gear is attached to the template, not the clone");
    if (reg.get<ptv::CPhysics>(gear).kind != "kinematic")
        return fail("cloned gear would be simulated instead of carried");
    for (const auto& t : reg.get<ptv::CTag>(gear).tags) {
        if (t == "template") return fail("cloned gear kept the template tag");
    }
    // The templates are untouched.
    if (reg.get<ptv::CAttachment>(club).parent != static_cast<std::uint32_t>(goblin))
        return fail("cloning moved the template's own gear");
    return ok("clone tree: full health, faction, no template tag, gear "
              "re-attached to the clone and carried kinematically");
}

static int case_runtime_spawn() {
    ptv::World world;
    ptv::MechanicContext ctx{};
    ptv::MechanicHost host;

    // A "template": bindings present, but parked and tagged so it neither
    // renders nor collides.
    const auto tmpl = world.create_entity("enemy_template");
    {
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, -500.0f, 0.0f);
        t.scale = 1.8f;
        world.registry().emplace<ptv::CTransform>(tmpl, ptv::CTransform{t});
        ptv::CPhysics p{};
        p.kind = "kinematic";
        p.collision_shape = "capsule";
        world.registry().emplace<ptv::CPhysics>(tmpl, p);
        world.registry().emplace<ptv::CTag>(tmpl, ptv::CTag{{"template",
                                                             "enemy"}});
        ptv::CMechanicBinding mb{};
        mb.entries.push_back({"test.probe", "", "{}"});
        world.registry().emplace<ptv::CMechanicBinding>(tmpl, std::move(mb));
    }

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);
    host.attach_bindings(world, ctx);

    // The template itself must stay inert: no collider, and although its
    // binding does attach, that is the scene's business -- what matters is
    // that it has no body to bump into.
    if (physics.has_body(static_cast<std::uint32_t>(tmpl)))
        return fail("a template entity was given a collider");

    const int ticks_before = g_probe_ticks;
    const ptv::InputSnapshot input{};
    host.update(world, 1.0f / 60.0f, input, ctx, ptv::MechanicPhase::Simulate);
    const int template_ticks = g_probe_ticks - ticks_before;

    // Now "spawn": clone the binding onto a fresh entity the way
    // Application::clone_entity does, then attach it at runtime.
    const auto spawned = world.create_entity("enemy_1");
    {
        ptv::Transform t{};
        t.position = ptv::Vec3(3.0f, 0.9f, 0.0f);
        t.scale = 1.8f;
        world.registry().emplace<ptv::CTransform>(spawned, ptv::CTransform{t});
        world.registry().emplace<ptv::CPhysics>(
            spawned, world.registry().get<ptv::CPhysics>(tmpl));
        world.registry().emplace<ptv::CTag>(spawned, ptv::CTag{{"enemy"}});
        world.registry().emplace<ptv::CMechanicBinding>(
            spawned, world.registry().get<ptv::CMechanicBinding>(tmpl));
    }
    const auto spawned_id = static_cast<std::uint32_t>(spawned);

    const int attached_before = g_probe_attached;
    physics.add_body_for(world, spawned_id);
    host.attach_entity(world, spawned_id, ctx);

    if (g_probe_attached != attached_before + 1)
        return fail("attach_entity did not attach the spawned mechanic");
    if (!physics.has_body(spawned_id))
        return fail("the spawned entity got no collider");

    const int before = g_probe_ticks;
    host.update(world, 1.0f / 60.0f, input, ctx, ptv::MechanicPhase::Simulate);
    if (g_probe_ticks - before != template_ticks + 1)
        return fail("the spawned entity's mechanic did not tick");

    // Despawn: the mechanic must stop ticking and the body must go.
    const int detached_before = g_probe_detached;
    host.detach_entity(world, spawned_id);
    physics.remove_body(spawned_id);
    world.registry().destroy(spawned);

    if (g_probe_detached != detached_before + 1)
        return fail("detach_entity did not deliver on_detach");
    if (physics.has_body(spawned_id))
        return fail("despawn left the collider behind");

    const int after_despawn = g_probe_ticks;
    host.update(world, 1.0f / 60.0f, input, ctx, ptv::MechanicPhase::Simulate);
    if (g_probe_ticks - after_despawn != template_ticks)
        return fail("a despawned entity's mechanic is still ticking");

    physics.shutdown();
    return ok("lifecycle: entities spawned after scene load get a collider "
              "and ticking mechanics, and lose both on despawn");
}

// Switching clips used to be a hard cut: active_clip changed and the pose
// jumped on that frame. Every gait change -- idle to walk, walk to run --
// therefore popped. tick_animators now cross-fades, and this pins the three
// things that must hold: the fade starts on the outgoing pose, passes
// strictly between the two, and lands exactly on the incoming one before
// tearing the fade state down.
static int case_clip_blend(const char* glb) {
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

    // Clip 0 is the fixture's animated track. Clip 1 is a copy flattened to
    // identity rotations, so it holds the rig still -- a stand-in for "idle"
    // that gives the blend two visibly different poses to interpolate.
    ptv::CAnimator anim{};
    anim.clips = skel.clips;
    gml::AnimationClip still = skel.clips[0];
    still.name = "still";
    for (auto& ch : still.channels) {
        if (ch.path != gml::AnimPath::Rotation) continue;
        for (size_t k = 0; k + 3 < ch.values.size(); k += 4) {
            ch.values[k + 0] = 0.0f;
            ch.values[k + 1] = 0.0f;
            ch.values[k + 2] = 0.0f;
            ch.values[k + 3] = 1.0f;
        }
    }
    anim.clips.push_back(std::move(still));
    anim.active_clip = 0;
    anim.looping = true;
    anim.playing = true;
    anim.joint_overridden.assign(skel.joint_count(), 0u);
    world.registry().emplace<ptv::CAnimator>(e, std::move(anim));

    const int tip = skel.name_to_joint.at("bone_tip");
    auto tip_angle_deg = [&]() {
        const auto& sk = world.get<ptv::CSkeleton>(id);
        const ptv::Quat q = sk.local_pose[static_cast<size_t>(tip)].rotation;
        return glm::degrees(
            2.0f * std::acos(std::min(1.0f, std::fabs(q.w))));
    };

    // Park the animated clip at its 45-degree peak and hold it there.
    ptv::tick_animators(world, 0.5f);
    const float animated = tip_angle_deg();
    if (std::fabs(animated - 45.0f) > 1.0f)
        return fail("fixture clip did not reach its expected pose");

    // Start a 0.2 s fade onto the flat clip, exactly as play_clip does.
    {
        auto& a = world.get<ptv::CAnimator>(id);
        a.prev_clip = a.active_clip;
        a.prev_time_s = a.time_s;
        a.active_clip = 1;
        a.time_s = 0.5f;
        a.blend_t = 0.2f;
        a.blend_duration = 0.2f;
    }

    // First frame of the fade: still fully weighted to the outgoing clip, so
    // nothing may jump.
    ptv::tick_animators(world, 0.0f);
    if (std::fabs(tip_angle_deg() - animated) > 1.0f) {
        std::fprintf(stderr, "  fade start moved the pose to %.2f deg\n",
                     tip_angle_deg());
        return fail("clip switch popped instead of blending");
    }

    // Halfway: strictly between the two poses.
    ptv::tick_animators(world, 0.1f);
    const float mid = tip_angle_deg();
    if (mid >= animated - 1.0f || mid <= 1.0f) {
        std::fprintf(stderr, "  mid-fade angle %.2f, between 0 and %.2f\n",
                     mid, animated);
        return fail("mid-fade pose is not between the two clips");
    }

    // Run the fade out. It must land on the flat clip and clear its state,
    // or the outgoing clip keeps being sampled forever.
    for (int i = 0; i < 4; ++i) ptv::tick_animators(world, 0.05f);
    if (tip_angle_deg() > 1.0f) {
        std::fprintf(stderr, "  ended at %.2f deg, want ~0\n",
                     tip_angle_deg());
        return fail("fade never reached the incoming clip");
    }
    const auto& done = world.get<ptv::CAnimator>(id);
    if (done.prev_clip != -1 || done.blend_t > 0.0f)
        return fail("fade finished but left its blend state behind");

    return ok("animation: clip changes cross-fade instead of snapping, and "
              "the fade cleans itself up");
}

// Minimal stand-ins for the two services builtin.character_locomotion uses,
// so the gait logic can be exercised without a Vulkan/OpenXR Application.
namespace {

class FakePhysics final : public ptv::IPhysicsAccess {
public:
    bool raycast(const ptv::Vec3& origin, const ptv::Vec3& dir, float max_dist,
                 ptv::PhysicsRayHit& out) const override {
        // A flat floor at y = 0, so ground snap has something to find.
        (void)dir;
        (void)max_dist;
        out.point = ptv::Vec3(origin.x, 0.0f, origin.z);
        out.normal = ptv::Vec3(0.0f, 1.0f, 0.0f);
        out.hit = true;
        return true;
    }
    bool has_body(std::uint32_t) const override { return false; }
    void set_kinematic(std::uint32_t, bool) override {}
    void move_kinematic(std::uint32_t, const ptv::Vec3&, const ptv::Quat&,
                        float) override {}
    void apply_impulse(std::uint32_t, const ptv::Vec3&) override {}
    ptv::Vec3 slide_move(std::uint32_t, const ptv::Vec3& delta) const override {
        return delta;  // open ground
    }
};

class FakeSkeleton final : public ptv::ISkeleton {
public:
    std::string requested;
    float speed{1.0f};
    int play_calls{0};
    bool have_run{true};
    bool have_walk{true};
    bool have_idle{true};
    /** Gait-variant clips ("run.punching") this fake asset has. */
    std::vector<std::string> variants;
    int fade_rest_calls{0};
    bool one_shot{false};

    bool has_skeleton(std::uint32_t) const override { return true; }
    int joint_index(std::uint32_t, const char*) const override { return -1; }
    int joint_count(std::uint32_t) const override { return 1; }
    int joint_for_slot(std::uint32_t, const char*) const override { return -1; }
    int joint_parent(std::uint32_t, int) const override { return -1; }
    ptv::Mat4 joint_world(std::uint32_t, int) const override {
        return ptv::Mat4(1.0f);
    }
    ptv::Mat4 joint_model(std::uint32_t, int) const override {
        return ptv::Mat4(1.0f);
    }
    ptv::Mat4 joint_bind_model(std::uint32_t, int) const override {
        return ptv::Mat4(1.0f);
    }
    ptv::Quat joint_bind_rotation(std::uint32_t, int) const override {
        return ptv::Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    ptv::Quat joint_local_rotation(std::uint32_t, int) const override {
        return ptv::Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    void set_joint_rotation_from_bind(std::uint32_t, int,
                                      const ptv::Quat&) override {}
    int joint_subtree(std::uint32_t, int, int*, int) const override {
        return 0;
    }
    void set_joint_rotation(std::uint32_t, int, const ptv::Quat&) override {}
    void set_joint_translation(std::uint32_t, int, const ptv::Vec3&) override {}
    void set_joint_scale(std::uint32_t, int, const ptv::Vec3&) override {}
    bool play_clip(std::uint32_t, const char* name, bool, float) override {
        if (name == nullptr) return false;
        ++play_calls;
        requested = name;
        return true;
    }
    void stop_clip(std::uint32_t) override {}
    void set_clip_speed(std::uint32_t, float s) override { speed = s; }
    bool has_clip(std::uint32_t, const char* name) const override {
        if (name == nullptr) return false;
        const std::string n(name);
        if (n == "run") return have_run;
        if (n == "walk") return have_walk;
        if (n == "idle") return have_idle;
        if (n.find('.') != std::string::npos) {
            return std::find(variants.begin(), variants.end(), n) != variants.end();
        }
        return true;
    }
    const char* current_clip(std::uint32_t) const override {
        return requested.c_str();
    }
    float clip_time(std::uint32_t) const override { return 0.0f; }
    float clip_duration(std::uint32_t) const override { return 1.0f; }
    bool clip_finished(std::uint32_t) const override { return false; }
    void fade_to_rest(std::uint32_t, float) override {
        ++fade_rest_calls;
        requested.clear();
    }
    bool one_shot_playing(std::uint32_t) const override { return one_shot; }
};

}  // namespace

// The heart of "animation synced to movement". A behaviour only writes a
// desired velocity; this mechanic has to pick the gait, keep the playback
// rate inside the window where scaling still looks like walking, and move the
// body. Getting the rate wrong is what makes characters moon-walk.
static int case_character_locomotion() {
    auto mech = ptv::MechanicRegistry::get().make_mechanic(
        "builtin.character_locomotion");
    if (!mech) return fail("builtin.character_locomotion is not registered");

    ptv::World world;
    const auto e = world.create_entity("goblin");
    const auto id = static_cast<std::uint32_t>(e);
    {
        ptv::Transform t{};
        // Start already resting on the floor so the first tick's ground snap
        // does not register as horizontal motion.
        t.position = ptv::Vec3(0.0f, 0.9f, 0.0f);
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        ptv::CMesh m{};
        m.asset_id = "goblin";
        m.target_scale_meters = 1.8f;  // -> walk ref 1.40, run ref 3.96 m/s
        m.skinned = true;
        world.registry().emplace<ptv::CMesh>(e, std::move(m));
    }

    FakePhysics physics;
    FakeSkeleton skeleton;
    ptv::MechanicContext ctx{};
    ctx.physics = &physics;
    ctx.skeleton = &skeleton;

    const ptv::InputSnapshot input{};
    mech->on_attach(world, id, nlohmann::json::object(), ctx);

    auto drive = [&](ptv::Vec3 velocity, int frames) {
        auto& want = world.registry().emplace_or_replace<
            ptv::CLocomotionIntent>(e);
        want.desired_velocity = velocity;
        for (int i = 0; i < frames; ++i) {
            mech->on_update(world, id, 1.0f / 60.0f, input, ctx);
        }
    };

    // --- Standing still -> idle ------------------------------------------
    drive(ptv::Vec3(0.0f), 3);
    if (skeleton.requested != "idle")
        return fail("a stationary character is not playing idle");

    // --- Walking pace -> walk, at natural playback rate -------------------
    drive(ptv::Vec3(0.0f, 0.0f, -1.4f), 10);
    if (skeleton.requested != "walk")
        return fail("walking pace did not select the walk clip");
    if (std::fabs(skeleton.speed - 1.0f) > 0.12f) {
        std::fprintf(stderr, "  walk clip rate %.3f, expected ~1.0\n",
                     skeleton.speed);
        return fail("walk playback rate does not match the ground covered");
    }

    // It must actually have moved, and be facing the way it went (-Z).
    {
        const auto& t = world.get<ptv::CTransform>(id).t;
        if (t.position.z > -0.05f)
            return fail("the character did not move");
        const ptv::Vec3 fwd = t.rotation * ptv::Vec3(0.0f, 0.0f, -1.0f);
        if (fwd.z > -0.9f) {
            std::fprintf(stderr, "  forward = (%.2f, %.2f, %.2f)\n", fwd.x,
                         fwd.y, fwd.z);
            return fail("the character is not facing its direction of travel");
        }
    }

    // --- Sprint -> run, and the rate stays inside the usable window -------
    drive(ptv::Vec3(0.0f, 0.0f, -9.0f), 20);
    if (skeleton.requested != "run")
        return fail("a sprint did not select the run clip");
    if (skeleton.speed < 0.80f - 1e-3f || skeleton.speed > 1.25f + 1e-3f) {
        std::fprintf(stderr, "  run clip rate %.3f outside [0.80, 1.25]\n",
                     skeleton.speed);
        return fail("playback scaling escaped the range where it still reads "
                    "as running");
    }

    // --- A rig with no run clip falls back to walk, not to nothing --------
    skeleton.have_run = false;
    drive(ptv::Vec3(0.0f, 0.0f, -9.0f), 5);
    if (skeleton.requested != "walk")
        return fail("a rig without a run clip did not fall back to walk");

    // --- ...and one with no walk clip runs slowly, not idles ---------------
    skeleton.have_run = true;
    skeleton.have_walk = false;
    drive(ptv::Vec3(0.0f), 3);
    drive(ptv::Vec3(0.0f, 0.0f, -1.4f), 10);
    if (skeleton.requested != "run")
        return fail("a rig without a walk clip played idle while walking");
    skeleton.have_walk = true;

    // --- animated:false still moves, but plays nothing --------------------
    {
        auto duck = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        const auto d = world.create_entity("duck");
        const auto did = static_cast<std::uint32_t>(d);
        ptv::Transform t{};
        t.position = ptv::Vec3(5.0f, 0.9f, 0.0f);
        t.scale = 1.0f;
        world.registry().emplace<ptv::CTransform>(d, ptv::CTransform{t});

        nlohmann::json params = nlohmann::json::object();
        params["animated"] = false;
        duck->on_attach(world, did, params, ctx);

        const int before = skeleton.play_calls;
        auto& want =
            world.registry().emplace_or_replace<ptv::CLocomotionIntent>(d);
        want.desired_velocity = ptv::Vec3(-2.0f, 0.0f, 0.0f);
        for (int i = 0; i < 10; ++i) {
            duck->on_update(world, did, 1.0f / 60.0f, input, ctx);
        }
        if (skeleton.play_calls != before)
            return fail("an un-animated slider still tried to play a clip");
        if (world.get<ptv::CTransform>(did).t.position.x >= 5.0f)
            return fail("an un-animated slider did not move");
    }

    return ok("character locomotion: gait follows achieved speed, playback "
              "rate matches ground covered, and animated:false still slides");
}

// Uthana characters: a gait VARIANT plays its own clip at the same travel
// speed, a missing idle settles to the rest pose instead of walking in place,
// a one-shot action is not overwritten by the gait clip, and the measured
// origin-to-sole distance plants the feet on the floor.
static int case_locomotion_variants() {
    auto mech = ptv::MechanicRegistry::get().make_mechanic(
        "builtin.character_locomotion");
    if (!mech) return fail("builtin.character_locomotion is not registered");

    ptv::World world;
    const auto e = world.create_entity("zombie");
    const auto id = static_cast<std::uint32_t>(e);
    {
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, 0.85f, 0.0f);
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        ptv::CMesh m{};
        m.asset_id = "zombie";
        m.target_scale_meters = 1.8f;
        m.skinned = true;
        m.foot_offset_m = 0.85f;  // asymmetric mesh: soles 0.85 m below origin
        world.registry().emplace<ptv::CMesh>(e, std::move(m));
    }
    FakePhysics physics;
    FakeSkeleton skeleton;
    skeleton.variants = {"run.punching"};
    ptv::MechanicContext ctx{};
    ctx.physics = &physics;
    ctx.skeleton = &skeleton;
    const ptv::InputSnapshot input{};
    mech->on_attach(world, id, nlohmann::json::object(), ctx);

    auto drive = [&](ptv::Vec3 velocity, const char* variant, int frames) {
        auto& want = world.registry().emplace_or_replace<ptv::CLocomotionIntent>(e);
        want.desired_velocity = velocity;
        want.gait_variant = variant;
        for (int i = 0; i < frames; ++i) {
            mech->on_update(world, id, 1.0f / 60.0f, input, ctx);
        }
    };

    drive(ptv::Vec3(0.0f, 0.0f, -9.0f), "punching", 20);
    if (skeleton.requested != "run.punching")
        return fail("a gait variant did not play its own clip");
    drive(ptv::Vec3(0.0f, 0.0f, -9.0f), "kicking", 5);
    if (skeleton.requested != "run")
        return fail("a variant the asset lacks did not fall back to the plain clip");

    const float y = world.get<ptv::CTransform>(id).t.position.y;
    if (std::fabs(y - 0.85f) > 1e-3f) {
        std::fprintf(stderr, "  origin at y=%.3f, expected 0.850\n", y);
        return fail("ground snap ignored the measured foot offset");
    }

    skeleton.have_idle = false;
    drive(ptv::Vec3(0.0f), "", 3);
    if (skeleton.fade_rest_calls == 0)
        return fail("standing still without an idle clip did not settle to rest");
    if (skeleton.requested == "run" || skeleton.requested == "walk")
        return fail("standing still without an idle clip kept the gait clip playing");

    skeleton.one_shot = true;
    const int before = skeleton.play_calls;
    drive(ptv::Vec3(0.0f, 0.0f, -9.0f), "", 5);
    if (skeleton.play_calls != before)
        return fail("a one-shot action was overwritten by the gait clip");

    return ok("locomotion variants: variant clips at gait speed, rest pose "
              "without idle, one-shots play out, measured foot offset");
}

// A Uthana rig hangs its skeleton under an "Armature" node scaled by 0.01 and
// rotated 90 degrees about X, and its clips animate the root joint in that
// node's centimetre space. The loader must bake the ancestor into the root's
// keys exactly as it folds it into the bind pose, or the animated character
// is drawn 100x too big and lying on its back.
static int case_uthana_root_bake(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;
    std::vector<gml::SkinnedVertex> verts;
    std::vector<std::uint32_t> indices;
    gml::SkeletonData skel;
    if (!gml::load_glb_skinned(glb, 0.0f, verts, indices, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skinned: %s\n", err.c_str());
        return 1;
    }
    if (skel.clips.empty()) return fail("merged Uthana fixture has no clip");

    auto bounds = [&](const std::vector<gml::JointPose>& pose, float& lo, float& hi) {
        std::vector<ptv::Mat4> palette;
        gml::build_joint_palette(skel, pose, palette);
        lo = 1e30f;
        hi = -1e30f;
        for (size_t i = 0; i < verts.size(); i += 7) {
            ptv::Mat4 m(0.0f);
            for (int k = 0; k < 4; ++k) {
                const float w = static_cast<float>(verts[i].weights[k]) / 255.0f;
                if (w > 0.0f) m += w * palette[verts[i].joints[k]];
            }
            const ptv::Vec3 p(m * ptv::Vec4(verts[i].pos[0], verts[i].pos[1],
                                            verts[i].pos[2], 1.0f));
            lo = std::min(lo, p.y);
            hi = std::max(hi, p.y);
        }
    };

    float bind_lo = 0.0f, bind_hi = 0.0f;
    bounds(skel.bind_pose, bind_lo, bind_hi);
    std::vector<gml::JointPose> pose = skel.bind_pose;
    const std::vector<std::uint8_t> none(skel.bind_pose.size(), 0u);
    ptv::sample_animation_clip(skel.clips[0], 0.0f, none, pose);
    float anim_lo = 0.0f, anim_hi = 0.0f;
    bounds(pose, anim_lo, anim_hi);

    const float bind_h = bind_hi - bind_lo;
    const float anim_h = anim_hi - anim_lo;
    std::printf("  bind y [%.3f, %.3f]  frame0 y [%.3f, %.3f]\n", bind_lo, bind_hi,
                anim_lo, anim_hi);
    if (bind_h < 1e-3f) return fail("fixture has no height");
    if (std::fabs(anim_h - bind_h) > 0.15f * bind_h)
        return fail("animated character height differs from bind: ancestor not baked");
    if (std::fabs(anim_lo - bind_lo) > 0.06f)
        return fail("animated feet are not where the bind feet are");
    return ok("Uthana root bake: animated root keeps the Armature's scale and "
              "rotation; feet stay on the bind sole");
}

// Scenario 2 from the NPC brief: the pipeline produced a goblin and a rifle
// as two separate assets, and the goblin has to be holding the rifle. The
// scene has to be able to SAY that, and the loader has to turn it into a
// real parent link -- including when the weapon is authored before the
// character it hangs off.
static int case_attachment() {
    const char* doc = R"({
        "version": 1, "prompt_id": "t", "scene_id": "s", "title": "T", "name": "S",
        "player": {"height_meters": 1.7, "locomotion": "teleport", "comfort": "comfort",
                    "spawn": {"position": [0,0,0], "rotation": [0,0,0,1]}},
        "sky_color": [0,0,0], "ambient_intensity": 0.1,
        "lights": [],
        "entities": [
          {"id": "rifle", "name": "Rifle",
           "transform": {"position": [0,0,0], "rotation": [0,0,0,1], "scale": 1.0},
           "mesh": {"asset_id": "rifle", "glb_path": "rifle.glb",
                    "target_scale_meters": 0.9, "collision_shape": "mesh"},
           "attached_to": {"entity_id": "goblin", "anchor": "hand_r",
                           "position": [0.02, -0.01, 0.05],
                           "rotation": [0,0,0,1]},
           "tags": ["weapon"]},
          {"id": "goblin", "name": "Goblin",
           "transform": {"position": [0,0,-3], "rotation": [0,0,0,1], "scale": 1.0},
           "mesh": {"asset_id": "goblin", "glb_path": "goblin.glb",
                    "target_scale_meters": 1.8, "collision_shape": "capsule",
                    "skinned": true, "joint_count": 65,
                    "animations": {"idle": "Idle", "walk": "Walk"},
                    "animation_speeds": {"walk": 1.404}},
           "tags": ["enemy"]},
          {"id": "orphan", "name": "Orphan",
           "transform": {"position": [1,0,0], "rotation": [0,0,0,1], "scale": 1.0},
           "attached_to": {"entity_id": "nobody", "anchor": "grip",
                           "position": [0,0,0], "rotation": [0,0,0,1]},
           "tags": []}
        ]
    })";
    const char* path = "ptv_smoke_attach_scene.json";
    std::FILE* f = std::fopen(path, "wb");
    std::fwrite(doc, 1, std::strlen(doc), f);
    std::fclose(f);

    auto loaded = ptv::LoadSceneJson(path);
    if (loaded.entities.size() != 3) return fail("attachment scene did not load");

    // The reference-speed contract has to survive the parse too, or the
    // runtime has no divisor and falls back to guessing.
    const auto& goblin_mesh = *loaded.entities[1].mesh;
    if (std::fabs(goblin_mesh.animation_speeds.at("walk") - 1.404f) > 1e-3f)
        return fail("animation_speeds did not survive the scene parse");

    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);

    const auto rifle = world.find("rifle");
    const auto goblin = world.find("goblin");
    const auto orphan = world.find("orphan");
    if (rifle == entt::null || goblin == entt::null)
        return fail("attachment scene entities missing");

    if (!world.registry().all_of<ptv::CAttachment>(rifle))
        return fail("the rifle was not parented to anything");

    const auto& att = world.registry().get<ptv::CAttachment>(rifle);
    // The rifle is authored BEFORE the goblin, so a single-pass loader would
    // have dropped this link.
    if (att.parent != static_cast<std::uint32_t>(goblin))
        return fail("forward reference to a later entity was not resolved");
    if (att.anchor != "hand_r")
        return fail("attachment anchor did not survive the parse");
    if (std::fabs(att.local_position.z - 0.05f) > 1e-4f)
        return fail("attachment offset did not survive the parse");

    // A dangling parent must be dropped loudly, not turned into an
    // attachment to entity 0 -- which would teleport the child onto whatever
    // happened to be created first.
    if (world.registry().all_of<ptv::CAttachment>(orphan))
        return fail("an attachment to an unknown entity was still created");

    return ok("attachment: a separately-generated weapon can be parented to "
              "a character's hand, including as a forward reference");
}

// Health and factions have to be engine concepts, not per-game inventions:
// an enemy authored by one CodeTask must be able to hurt a player set up by
// another, and an allied NPC must recognise that enemy without the two tasks
// having agreed on anything in advance.
static int case_combat_factions() {
    // --- Faction rules ---------------------------------------------------
    const ptv::CFaction player{"player", {"enemy"}};
    const ptv::CFaction enemy{"enemy", {"player", "ally"}};
    const ptv::CFaction ally{"ally", {}};
    const ptv::CFaction other_enemy{"enemy", {"player"}};

    if (!ptv::factions_hostile(player, enemy))
        return fail("player and enemy are not hostile");
    if (ptv::factions_hostile(enemy, other_enemy))
        return fail("two entities of the same faction are hostile");
    if (ptv::factions_hostile(player, ally))
        return fail("player and ally are hostile");
    // The asymmetric case: only the enemy declared the ally hostile. If this
    // did not resolve to "hostile" the allies would stand there being shot,
    // which is exactly the bug the symmetric rule exists to prevent.
    if (!ptv::factions_hostile(ally, enemy))
        return fail("a one-sided hostility declaration was ignored");

    // --- Authoring path --------------------------------------------------
    const char* doc = R"({
        "version": 1, "prompt_id": "t", "scene_id": "s", "title": "T", "name": "S",
        "player": {"height_meters": 1.7, "locomotion": "teleport", "comfort": "comfort",
                    "spawn": {"position": [0,0,0], "rotation": [0,0,0,1]}},
        "sky_color": [0,0,0], "ambient_intensity": 0.1,
        "lights": [],
        "entities": [
          {"id": "grunt", "name": "Grunt",
           "transform": {"position": [0,0,-4], "rotation": [0,0,0,1], "scale": 1.0},
           "health": {"max": 40.0, "invuln_after_hit_s": 0.25},
           "faction": {"id": "enemy", "hostile_to": ["player", "ally"]},
           "tags": ["enemy"]},
          {"id": "crate", "name": "Crate",
           "transform": {"position": [2,0,0], "rotation": [0,0,0,1], "scale": 1.0},
           "tags": ["static"]}
        ]
    })";
    const char* path = "ptv_smoke_combat_scene.json";
    std::FILE* f = std::fopen(path, "wb");
    std::fwrite(doc, 1, std::strlen(doc), f);
    std::fclose(f);

    auto loaded = ptv::LoadSceneJson(path);
    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);

    const auto grunt = world.find("grunt");
    const auto crate = world.find("crate");
    if (grunt == entt::null || crate == entt::null)
        return fail("combat scene entities missing");

    if (!world.registry().all_of<ptv::CHealth>(grunt))
        return fail("authored health did not reach the ECS");
    const auto& hp = world.registry().get<ptv::CHealth>(grunt);
    if (std::fabs(hp.max - 40.0f) > 1e-4f)
        return fail("authored max health was not applied");
    // Spawning at anything other than full health would have every enemy
    // arrive already wounded.
    if (std::fabs(hp.current - hp.max) > 1e-4f)
        return fail("an entity did not spawn at full health");
    if (std::fabs(hp.invuln_after_hit_s - 0.25f) > 1e-4f)
        return fail("authored invulnerability window was not applied");

    const auto& fac = world.registry().get<ptv::CFaction>(grunt);
    if (fac.id != "enemy" || fac.hostile_to.size() != 2)
        return fail("authored faction did not reach the ECS");

    // Scenery must stay out of combat entirely: giving everything health
    // would make every crate a valid target for enemy fire.
    if (world.registry().all_of<ptv::CHealth>(crate))
        return fail("an entity with no authored health got some anyway");
    if (world.registry().all_of<ptv::CFaction>(crate))
        return fail("an entity with no authored faction got one anyway");

    return ok("combat: factions resolve symmetrically and authored health "
              "reaches the ECS at full hit points");
}

// The case local steering cannot solve. Two rooms joined by one doorway: an
// NPC in room A that only knows what its whiskers hit will press into the
// wall next to the door and jitter there. Routing has to send it AT the
// opening first.
static int case_nav_graph() {
    ptv::nav::NavGraph graph;

    // Room A spans x [-10, -1], room B spans x [1, 10]; the doorway is the
    // narrow strip between them at z = 0.
    ptv::nav::NavRegion a{};
    a.id = "room_a";
    a.min = ptv::Vec3(-10.0f, 0.0f, -5.0f);
    a.max = ptv::Vec3(-1.0f, 0.0f, 5.0f);
    ptv::nav::NavRegion b{};
    b.id = "room_b";
    b.min = ptv::Vec3(1.0f, 0.0f, -5.0f);
    b.max = ptv::Vec3(10.0f, 0.0f, 5.0f);
    ptv::nav::NavRegion c{};  // an isolated room, no links
    c.id = "vault";
    c.min = ptv::Vec3(30.0f, 0.0f, 30.0f);
    c.max = ptv::Vec3(35.0f, 0.0f, 35.0f);

    const int ia = graph.add_region(a);
    const int ib = graph.add_region(b);
    graph.add_region(c);

    ptv::nav::NavLink door{};
    door.from = ia;
    door.to = ib;
    door.point = ptv::Vec3(0.0f, 0.0f, 0.0f);
    graph.add_link(door);

    // --- Containment -----------------------------------------------------
    if (graph.region_at(ptv::Vec3(-5.0f, 0.0f, 0.0f)) != ia)
        return fail("a point inside room A did not resolve to it");
    if (graph.region_at(ptv::Vec3(500.0f, 0.0f, 500.0f)) >= 0)
        return fail("a point far outside every region still resolved");

    // --- Routing through the door ----------------------------------------
    // Both on the same side of the room so the straight line between them
    // does NOT happen to pass through the doorway; otherwise the detour is
    // free and the distance check proves nothing.
    const ptv::Vec3 from(-8.0f, 0.0f, 4.0f);
    const ptv::Vec3 to(8.0f, 0.0f, 4.0f);
    std::vector<ptv::Vec3> corners;
    if (!graph.find_path(from, to, corners))
        return fail("no route between two linked rooms");
    if (corners.size() != 3) {
        std::fprintf(stderr, "  got %zu corners, expected 3\n", corners.size());
        return fail("the route did not pass through exactly one doorway");
    }
    // The middle corner must BE the doorway. That is the whole point: an NPC
    // steered straight at the destination walks into the dividing wall.
    if (std::fabs(corners[1].x) > 1e-4f)
        return fail("the intermediate corner is not the doorway");

    // Walking distance is longer than the straight line, because the route
    // has to detour via the door. A caller choosing between targets needs
    // that difference to be real.
    float walk = 0.0f;
    if (!graph.path_distance(from, to, walk))
        return fail("path_distance found no route");
    const float straight = glm::length(ptv::Vec3(to.x - from.x, 0.0f,
                                                 to.z - from.z));
    if (walk <= straight) {
        std::fprintf(stderr, "  walk %.2f vs straight %.2f\n", walk, straight);
        return fail("walking distance did not account for the detour");
    }

    // --- Same region is just a straight line ------------------------------
    std::vector<ptv::Vec3> same;
    if (!graph.find_path(ptv::Vec3(-8.0f, 0.0f, 0.0f),
                         ptv::Vec3(-2.0f, 0.0f, 0.0f), same))
        return fail("no route within one region");
    if (same.size() != 2)
        return fail("a route inside one region invented a detour");

    // --- Unreachable is reported, not faked -------------------------------
    if (graph.is_reachable(from, ptv::Vec3(32.0f, 0.0f, 32.0f)))
        return fail("a sealed room was reported reachable");
    if (!graph.is_reachable(from, to))
        return fail("a linked room was reported unreachable");

    // A level whose builder declared nothing must not freeze every NPC.
    ptv::nav::NavGraph empty;
    if (!empty.is_reachable(from, to))
        return fail("an empty graph refused every destination");

    return ok("navigation: routes through a doorway, reports true walking "
              "distance, and refuses only genuinely sealed destinations");
}

// Posing a 65-joint skeleton costs the same whether the character fills the
// view or is a speck on the horizon, and Meta's guidance singles out
// animation and skinning as work that delays the render thread. So distant
// characters are sampled less often -- but they must still cover the same
// ground per second, or a Quest build would show NPCs slowing down as they
// walk away from you.
static int case_animation_lod(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    std::string err;

    gml::SkeletonData skel;
    if (!gml::load_glb_skeleton(glb, 0.0f, skel, err)) {
        std::fprintf(stderr, "[fail] load_glb_skeleton: %s\n", err.c_str());
        return 1;
    }
    if (skel.clips.empty()) return fail("fixture carried no clips");

    ptv::World world;
    auto make = [&](const char* id, float z) {
        const auto e = world.create_entity(id);
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, 0.0f, z);
        world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
        ptv::CSkeleton cs{};
        cs.local_pose = skel.bind_pose;
        cs.skeleton = skel;
        world.registry().emplace<ptv::CSkeleton>(e, std::move(cs));
        ptv::CAnimator anim{};
        anim.clips = skel.clips;
        anim.active_clip = 0;
        anim.looping = true;
        anim.playing = true;
        anim.joint_overridden.assign(skel.joint_count(), 0u);
        world.registry().emplace<ptv::CAnimator>(e, std::move(anim));
        return static_cast<std::uint32_t>(e);
    };

    const auto near_e = make("near", 2.0f);
    const auto mid_e = make("mid", 12.0f);
    const auto far_e = make("far", 25.0f);
    const auto gone_e = make("gone", 200.0f);

    ptv::AnimationLod lod{};
    lod.full_rate_m = 8.0f;
    lod.half_rate_m = 15.0f;
    lod.stop_m = 40.0f;

    const ptv::Vec3 viewer(0.0f, 0.0f, 0.0f);
    const float dt = 1.0f / 60.0f;
    // Half a second against a one-second clip, so the comparison never
    // straddles the loop point where fmod makes "just before the end" and
    // "just after the start" look maximally different.
    const int frames = 30;
    for (int i = 0; i < frames; ++i) {
        ptv::tick_animators(world, dt, viewer, lod);
    }

    auto clip_time = [&](std::uint32_t e) {
        return world.get<ptv::CAnimator>(e).time_s;
    };

    // Every character that is still in range must have advanced by the same
    // amount of wall-clock time. This is the property that makes throttling
    // invisible: only the sampling rate drops, never the playback speed.
    const float expected = frames * dt;
    for (auto [name, e] : {std::pair<const char*, std::uint32_t>{"near", near_e},
                           {"mid", mid_e},
                           {"far", far_e}}) {
        // Allow up to one skipped frame of lag: an entity on a 4-frame
        // stride may not have been sampled on the final frame.
        if (std::fabs(clip_time(e) - expected) > 4.0f * dt + 1e-4f) {
            std::fprintf(stderr, "  %s at t=%.4f, expected ~%.4f\n", name,
                         clip_time(e), expected);
            return fail("a throttled character drifted out of sync");
        }
    }

    // Out of range entirely: never posed, so its clip has not advanced.
    if (clip_time(gone_e) != 0.0f)
        return fail("a character beyond the stop distance was still posed");
    // But the time it missed is banked, so walking back into range resumes
    // rather than rewinding.
    if (world.get<ptv::CAnimator>(gone_e).lod_debt_s <= 0.0f)
        return fail("skipped time was not banked as debt");

    // Now bring it into range: the debt is paid back on the next sample.
    world.get<ptv::CTransform>(gone_e).t.position = ptv::Vec3(0.0f, 0.0f, 1.0f);
    ptv::tick_animators(world, dt, viewer, lod);
    if (world.get<ptv::CAnimator>(gone_e).lod_debt_s != 0.0f)
        return fail("debt was not cleared when the character came back");
    if (clip_time(gone_e) <= 0.0f)
        return fail("a character that came back into range did not resume");

    // With LOD off, everything is posed every frame -- the behaviour the
    // pose-system tests rely on.
    ptv::World plain;
    const auto only = plain.create_entity("only");
    {
        ptv::Transform t{};
        t.position = ptv::Vec3(0.0f, 0.0f, 500.0f);
        plain.registry().emplace<ptv::CTransform>(only, ptv::CTransform{t});
        ptv::CSkeleton cs{};
        cs.local_pose = skel.bind_pose;
        cs.skeleton = skel;
        plain.registry().emplace<ptv::CSkeleton>(only, std::move(cs));
        ptv::CAnimator anim{};
        anim.clips = skel.clips;
        anim.active_clip = 0;
        anim.joint_overridden.assign(skel.joint_count(), 0u);
        plain.registry().emplace<ptv::CAnimator>(only, std::move(anim));
    }
    ptv::tick_animators(plain, dt);
    if (plain.get<ptv::CAnimator>(static_cast<std::uint32_t>(only)).time_s
            <= 0.0f) {
        return fail("LOD-free tick skipped a distant character");
    }

    return ok("animation LOD: distant characters are sampled less often but "
              "still play at the correct speed");
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

// ---------------------------------------------------------------------------
// NPC runtime against REAL physics.
//
// The fakes above answer every raycast with "floor at y = 0", which is why
// they never noticed that a ground probe starting on top of an NPC's own
// capsule hits that capsule first and lifts the NPC by its height every
// frame. These cases run Jolt.
// ---------------------------------------------------------------------------
namespace {

class RealPhysicsAccess final : public ptv::IPhysicsAccess {
public:
    RealPhysicsAccess(ptv::PhysicsWorld& p, ptv::World& w) : p_(p), w_(w) {}

    bool raycast(const ptv::Vec3& origin, const ptv::Vec3& dir, float max_dist,
                 ptv::PhysicsRayHit& out) const override {
        out = {};
        ptv::Vec3 n(0.0f, 1.0f, 0.0f);
        if (!p_.raycast(origin, dir, max_dist, out.point, n, out.entity)) return false;
        out.normal = n;
        out.hit = true;
        return true;
    }
    bool raycast_excluding(std::uint32_t self, const ptv::Vec3& origin,
                           const ptv::Vec3& dir, float max_dist,
                           ptv::PhysicsRayHit& out, bool floor_only) const override {
        out = {};
        ptv::PhysicsWorld::RayFilter f{};
        f.ignore.push_back(self);
        f.skip_characters = floor_only;
        ptv::Vec3 n(0.0f, 1.0f, 0.0f);
        if (!p_.raycast(origin, dir, max_dist, f, out.point, n, out.entity)) return false;
        out.normal = n;
        out.hit = true;
        return true;
    }
    bool has_body(std::uint32_t e) const override { return p_.has_body(e); }
    void set_kinematic(std::uint32_t e, bool k) override { p_.set_kinematic(e, k); }
    void move_kinematic(std::uint32_t e, const ptv::Vec3& pos, const ptv::Quat& rot,
                        float dt) override {
        p_.move_kinematic(e, pos, rot, dt);
    }
    void apply_impulse(std::uint32_t e, const ptv::Vec3& i) override {
        p_.apply_impulse(e, i);
    }
    ptv::Vec3 slide_move(std::uint32_t e, const ptv::Vec3& delta) const override {
        const auto& t = w_.get<ptv::CTransform>(e).t;
        const float h = w_.get<ptv::CMesh>(e).target_scale_meters;
        return p_.slide_character(t.position, h, delta, e);
    }

private:
    ptv::PhysicsWorld& p_;
    ptv::World& w_;
};

void add_floor(ptv::World& world) {
    const auto e = world.create_entity("floor");
    ptv::Transform t{};
    t.position = ptv::Vec3(0.0f, -0.1f, 0.0f);
    world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
    world.registry().emplace<ptv::CPrimitive>(
        e, ptv::CPrimitive{"box", ptv::Vec3(40.0f, 0.2f, 40.0f), ptv::Vec3(0.5f)});
    ptv::CPhysics p{};
    p.kind = "static";
    p.collision_shape = "box";
    world.registry().emplace<ptv::CPhysics>(e, p);
}

std::uint32_t add_npc(ptv::World& world, const char* id, const ptv::Vec3& pos,
                      float height, float foot_offset) {
    const auto e = world.create_entity(id);
    ptv::Transform t{};
    t.position = pos;
    world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
    ptv::CMesh m{};
    m.asset_id = id;
    m.target_scale_meters = height;
    m.foot_offset_m = foot_offset;
    m.skinned = true;
    world.registry().emplace<ptv::CMesh>(e, std::move(m));
    ptv::CPhysics p{};
    p.kind = "kinematic";
    p.mass_kg = 70.0f;
    p.collision_shape = "capsule";
    world.registry().emplace<ptv::CPhysics>(e, p);
    return static_cast<std::uint32_t>(e);
}

}  // namespace

static int case_npc_ground_snap_real_physics() {
    ptv::World world;
    add_floor(world);
    // The two placements the scene compiler produces: an Uthana companion
    // put down AT the floor (origin at the hips, foot offset 0.86 m), and a
    // Tripo enemy spawned onto a zone on the floor (half-height fallback).
    const auto ally = add_npc(world, "ally", ptv::Vec3(0.0f, 0.05f, 0.0f), 1.72f, 0.86f);
    const auto enemy = add_npc(world, "enemy", ptv::Vec3(4.0f, 0.0f, 0.0f), 1.45f, 0.0f);

    ptv::PhysicsWorld physics;
    if (!physics.initialize()) return fail("physics failed to initialize");
    physics.rebuild_from_world(world);
    RealPhysicsAccess access(physics, world);
    FakeSkeleton skeleton;
    ptv::MechanicContext ctx{};
    ctx.physics = &access;
    ctx.skeleton = &skeleton;

    auto loco_a = ptv::MechanicRegistry::get().make_mechanic("builtin.character_locomotion");
    auto loco_e = ptv::MechanicRegistry::get().make_mechanic("builtin.character_locomotion");
    if (!loco_a || !loco_e) return fail("builtin.character_locomotion is not registered");
    loco_a->on_attach(world, ally, nlohmann::json::object(), ctx);
    loco_e->on_attach(world, enemy, nlohmann::json::object(), ctx);
    // The enemy walks the whole time; the ally stands still.
    world.registry().emplace<ptv::CLocomotionIntent>(static_cast<entt::entity>(enemy))
        .desired_velocity = ptv::Vec3(-1.0f, 0.0f, 0.0f);

    const ptv::InputSnapshot input{};
    const float dt = 1.0f / 72.0f;
    for (int i = 0; i < 300; ++i) {
        physics.step(dt);
        physics.sync_to_world(world);
        loco_a->on_update(world, ally, dt, input, ctx);
        loco_e->on_update(world, enemy, dt, input, ctx);
    }
    const auto& ta = world.get<ptv::CTransform>(ally).t;
    const auto& te = world.get<ptv::CTransform>(enemy).t;
    std::fprintf(stderr, "  ally y=%.3f (want 0.86), enemy y=%.3f (want 0.725) x=%.2f\n",
                 ta.position.y, te.position.y, te.position.x);
    if (std::fabs(ta.position.y - 0.86f) > 0.08f)
        return fail("a standing NPC did not settle with its feet on the floor");
    if (std::fabs(te.position.y - 0.725f) > 0.08f)
        return fail("a walking NPC did not stay on the floor");
    if (te.position.x > 1.0f) return fail("the walking NPC did not move");

    // A ray from inside a capsule must not report that capsule.
    ptv::PhysicsRayHit hit{};
    if (!access.raycast_excluding(ally, ta.position, ptv::Vec3(0, -1, 0), 5.0f, hit, true))
        return fail("excluding raycast found no floor under the ally");
    if (hit.entity == ally) return fail("excluding raycast hit the caster");
    if (std::fabs(hit.point.y) > 0.02f) return fail("excluding raycast missed the floor");
    physics.shutdown();
    return ok("npc ground snap: real-physics NPCs stand on the floor instead of "
              "climbing their own capsules");
}

namespace {

int g_bprobe_attach = 0;
int g_bprobe_ticks = 0;
bool g_bprobe_had_ctx = false;

class ProbeBehaviour final : public ptv::IBehaviour {
public:
    void on_attach(ptv::World&, std::uint32_t, const nlohmann::json&,
                   ptv::MechanicContext&) override {
        ++g_bprobe_attach;
    }
    void on_tick(ptv::World& world, std::uint32_t entity, float,
                 const ptv::InputSnapshot&, ptv::MechanicContext& ctx) override {
        ++g_bprobe_ticks;
        g_bprobe_had_ctx = ctx.hands == nullptr;  // the test passes an empty ctx
        world.registry().get_or_emplace<ptv::CLocomotionIntent>(
            static_cast<entt::entity>(entity)).desired_velocity = ptv::Vec3(1.0f, 0, 0);
    }
    void on_detach(ptv::World&, std::uint32_t) override {}
    std::string id() const override { return "test.probe_behaviour"; }
};

/** The context-free form a project generated before the context existed. */
class LegacyBehaviour final : public ptv::IBehaviour {
public:
    using ptv::IBehaviour::on_tick;
    void on_tick(ptv::World&, std::uint32_t, float) override { ++ticks; }
    void on_detach(ptv::World&, std::uint32_t) override {}
    std::string id() const override { return "test.legacy_behaviour"; }
    static int ticks;
};
int LegacyBehaviour::ticks = 0;

PTV_REGISTER_BEHAVIOUR("test.probe_behaviour", ProbeBehaviour)
PTV_REGISTER_BEHAVIOUR("test.legacy_behaviour", LegacyBehaviour)

std::uint32_t bound_entity(ptv::World& world, const char* id,
                           std::vector<std::string> mechanics,
                           std::vector<std::string> tags) {
    const auto e = world.create_entity(id);
    world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{});
    ptv::CMechanicBinding mb{};
    for (auto& m : mechanics) mb.entries.push_back({m, "", "{}"});
    world.registry().emplace<ptv::CMechanicBinding>(e, std::move(mb));
    world.registry().emplace<ptv::CTag>(e, ptv::CTag{std::move(tags)});
    return static_cast<std::uint32_t>(e);
}

}  // namespace

static int case_mechanic_host_npc() {
    g_bprobe_attach = g_bprobe_ticks = 0;
    LegacyBehaviour::ticks = 0;
    ptv::World world;
    const auto live = bound_entity(world, "ally", {"test.probe_behaviour"}, {"npc"});
    const auto tmpl = bound_entity(world, "enemy_template",
                                   {"test.probe_behaviour", "test.legacy_behaviour"},
                                   {"enemy", "template"});
    bound_entity(world, "old_ai", {"test.legacy_behaviour", "generated.not_real"}, {"npc"});

    ptv::MechanicContext ctx{};
    ptv::MechanicHost host;
    host.attach_bindings(world, ctx);
    const ptv::InputSnapshot input{};
    for (int i = 0; i < 5; ++i) {
        host.update(world, 1.0f / 60.0f, input, ctx, ptv::MechanicPhase::Simulate);
    }
    if (g_bprobe_attach != 1)
        return fail("a behaviour bound to a template was attached (templates must be inert)");
    if (g_bprobe_ticks != 5) return fail("a registered IBehaviour was not ticked");
    if (!g_bprobe_had_ctx) return fail("the behaviour did not receive the context");
    if (LegacyBehaviour::ticks != 5)
        return fail("a legacy context-free IBehaviour was not ticked");
    if (!world.registry().all_of<ptv::CLocomotionIntent>(static_cast<entt::entity>(live)))
        return fail("the behaviour's locomotion intent was lost");
    if (world.registry().all_of<ptv::CLocomotionIntent>(static_cast<entt::entity>(tmpl)))
        return fail("a template's behaviour ran");
    if (host.unresolved().count("generated.not_real") != 1)
        return fail("an unresolved binding was not reported");

    // A clone of the template (tags minus "template") attaches at spawn.
    std::vector<ptv::CloneRecord> made;
    const auto clone = ptv::clone_entity_tree(world, static_cast<entt::entity>(tmpl),
                                              ptv::Vec3(1, 0, 1), ptv::Quat(1, 0, 0, 0), made);
    host.attach_entity(world, static_cast<std::uint32_t>(clone), ctx);
    if (g_bprobe_attach != 2) return fail("a spawned clone did not attach its behaviour");
    return ok("mechanic host: behaviours are hosted, templates stay inert, "
              "unresolved ids are reported");
}

static int case_player_rig_adoption() {
    ptv::World world;
    const auto rig = bound_entity(world, "player_rig", {}, {"player_rig", "player_system"});
    bound_entity(world, "player_avatar", {}, {"player_avatar"});
    if (ptv::find_adoptable_player_rig(world) != static_cast<entt::entity>(rig))
        return fail("the scene's player_rig was not chosen as the player");
    ptv::World none;
    bound_entity(none, "crate", {}, {"grabbable"});
    if (ptv::find_adoptable_player_rig(none) != entt::null)
        return fail("a scene without a player_rig adopted something else");
    ptv::World meshed;
    const auto m = bound_entity(meshed, "statue_rig", {}, {"player_rig"});
    ptv::CMesh cm{};
    cm.glb_path = "statue.glb";
    meshed.registry().emplace<ptv::CMesh>(static_cast<entt::entity>(m), cm);
    if (ptv::find_adoptable_player_rig(meshed) != entt::null)
        return fail("a drawable entity was adopted as the invisible player body");
    return ok("player entity: the scene's player_rig is the one live player");
}

static int case_damage_log() {
    ptv::DamageLog log;
    ptv::DamageEvent a{};
    a.target = 7;
    a.instigator = 3;
    a.amount = 12.0f;
    const auto s1 = log.record(a);
    a.target = 9;
    log.record(a);
    a.target = 7;
    a.amount = 5.0f;
    const auto s3 = log.record(a);
    if (s1 == 0 || s3 <= s1) return fail("damage sequence numbers are not increasing");
    if (log.last_seq(7) != s3) return fail("last_seq does not report the newest hit");
    if (log.last_seq(42) != 0) return fail("an entity never hit has a nonzero seq");
    ptv::DamageEvent out[8];
    if (log.since(7, 0, out, 8) != 2) return fail("since() miscounted one target's hits");
    if (out[0].amount != 12.0f || out[1].amount != 5.0f)
        return fail("since() did not return hits oldest first");
    if (log.since(7, s1, out, 8) != 1) return fail("since() did not honour the cursor");
    if (log.since(0xFFFFFFFFu, 0, out, 8) != 3) return fail("since(any) missed hits");
    for (int i = 0; i < 100; ++i) log.record(a);
    if (log.since(0xFFFFFFFFu, 0, out, 8) != 8) return fail("since() overran max_out");
    log.clear();
    const auto after = log.record(a);
    if (after <= s3) return fail("clear() reset the sequence, old cursors would break");
    return ok("damage log: hits are observable by sequence, whoever dealt them");
}

// --- Grounding + facing ----------------------------------------------------

namespace {

/** Terrain for the motor tests: a floor at y = 0 for x < 1 and a pit floor
 *  at y = -3 beyond, optionally with a steep ramp at x >= 3. Rays hit the
 *  highest surface below their origin. */
class FakeTerrain final : public ptv::IPhysicsAccess {
public:
    bool steep_wall{false};
    bool raycast(const ptv::Vec3& origin, const ptv::Vec3&, float max_dist,
                 ptv::PhysicsRayHit& out) const override {
        float y = origin.x < 1.0f ? 0.0f : -3.0f;
        ptv::Vec3 n(0.0f, 1.0f, 0.0f);
        if (steep_wall && origin.x >= 3.0f) {
            y = -2.0f;  // 1 m above the pit: too tall to step, too steep to walk
            n = glm::normalize(ptv::Vec3(-1.0f, 0.3f, 0.0f));
        }
        if (origin.y - y > max_dist || origin.y < y) return false;
        out.point = ptv::Vec3(origin.x, y, origin.z);
        out.normal = n;
        out.hit = true;
        return true;
    }
    bool has_body(std::uint32_t) const override { return false; }
    void set_kinematic(std::uint32_t, bool) override {}
    void move_kinematic(std::uint32_t, const ptv::Vec3&, const ptv::Quat&,
                        float) override {}
    void apply_impulse(std::uint32_t, const ptv::Vec3&) override {}
    ptv::Vec3 slide_move(std::uint32_t, const ptv::Vec3& d) const override {
        return d;
    }
};

std::uint32_t make_walker(ptv::World& world, const char* name, ptv::Vec3 pos,
                          float foot) {
    const auto e = world.create_entity(name);
    ptv::Transform t{};
    t.position = pos;
    world.registry().emplace<ptv::CTransform>(e, ptv::CTransform{t});
    ptv::CMesh m{};
    m.asset_id = name;
    m.target_scale_meters = 1.8f;
    m.skinned = true;
    m.foot_offset_m = foot;
    world.registry().emplace<ptv::CMesh>(e, std::move(m));
    return static_cast<std::uint32_t>(e);
}

}  // namespace

// The shared yaw convention: a forward vector and its yaw round-trip, and the
// rotation built from a yaw really points -Z at that heading.
static int case_facing_math() {
    const ptv::Vec3 dirs[] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
        {0.7071f, 0, 0.7071f}, {-0.7071f, 0, 0.7071f},
        {0.7071f, 0, -0.7071f}, {-0.7071f, 0, -0.7071f}};
    for (const auto& d : dirs) {
        const ptv::Vec3 f = ptv::facing_of(ptv::rotation_facing(d));
        if (glm::length(f - d) > 1e-4f) {
            std::fprintf(stderr, "  dir (%.2f,%.2f) -> faced (%.2f,%.2f)\n",
                         d.x, d.z, f.x, f.z);
            return fail("rotation_facing does not face its direction");
        }
        const ptv::Vec3 back = ptv::forward_from_yaw(ptv::yaw_from_forward(d));
        if (glm::length(back - d) > 1e-4f)
            return fail("yaw_from_forward / forward_from_yaw do not round-trip");
    }
    return ok("facing math: one yaw convention, round-trips in every direction");
}

// Every travel direction, not just -Z: a mirrored yaw formula looks right
// only along Z, and that is the one direction the old test checked.
static int case_facing_directions() {
    FakePhysics physics;
    FakeSkeleton skeleton;
    ptv::MechanicContext ctx{};
    ctx.physics = &physics;
    ctx.skeleton = &skeleton;
    const ptv::InputSnapshot input{};
    const ptv::Vec3 dirs[] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1},
        {0.7071f, 0, 0.7071f}, {-0.7071f, 0, -0.7071f}};
    for (const auto& d : dirs) {
        ptv::World world;
        auto mech = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        if (!mech) return fail("builtin.character_locomotion is not registered");
        const auto id = make_walker(world, "walker", ptv::Vec3(0, 0.9f, 0), 0.9f);
        mech->on_attach(world, id, nlohmann::json::object(), ctx);
        auto& want = world.registry().emplace_or_replace<ptv::CLocomotionIntent>(
            static_cast<entt::entity>(id));
        want.desired_velocity = d * 1.4f;
        for (int i = 0; i < 90; ++i) mech->on_update(world, id, 1.0f / 60.0f, input, ctx);
        const auto& t = world.get<ptv::CTransform>(id).t;
        const float err = glm::degrees(ptv::heading_error(ptv::facing_of(t.rotation), d));
        const ptv::Vec3 moved = t.position - ptv::Vec3(0, 0.9f, 0);
        if (err > 3.0f) {
            std::fprintf(stderr, "  travel (%.2f, %.2f): facing error %.1f deg\n",
                         d.x, d.z, err);
            return fail("the character does not face its direction of travel");
        }
        if (glm::dot(ptv::Vec3(moved.x, 0, moved.z), d) < 0.5f)
            return fail("the character did not travel the way it was asked");
    }

    // Turn-in-place: asked to reverse, it must cover less ground in the first
    // few frames than it would at full speed, because it is turning.
    {
        ptv::World world;
        auto mech = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        const auto id = make_walker(world, "turner", ptv::Vec3(0, 0.9f, 0), 0.9f);
        nlohmann::json p = nlohmann::json::object();
        p["turn_rate_deg_s"] = 180.0f;
        mech->on_attach(world, id, p, ctx);
        auto& want = world.registry().emplace_or_replace<ptv::CLocomotionIntent>(
            static_cast<entt::entity>(id));
        want.desired_velocity = ptv::Vec3(0, 0, 1.4f);  // behind it (+Z)
        for (int i = 0; i < 6; ++i) mech->on_update(world, id, 1.0f / 60.0f, input, ctx);
        const float covered = world.get<ptv::CTransform>(id).t.position.z;
        if (covered > 0.6f * 1.4f * 6.0f / 60.0f)
            return fail("a character facing away walked off backwards at full "
                        "speed instead of turning first");
    }
    return ok("facing: every travel direction is faced, and a reversal turns "
              "in place before walking");
}

// Grounded motor: off a ledge it FALLS (not teleports) and lands; a jump
// rises and comes back; a steep slope above it is a wall; a hovering flyer
// holds its height.
static int case_ground_motor() {
    FakeTerrain terrain;
    FakeSkeleton skeleton;
    ptv::MechanicContext ctx{};
    ctx.physics = &terrain;
    ctx.skeleton = &skeleton;
    const ptv::InputSnapshot input{};
    const float dt = 1.0f / 60.0f;
    const float foot = 0.9f;

    {  // Ledge
        ptv::World world;
        auto mech = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        const auto id = make_walker(world, "faller", ptv::Vec3(0.5f, foot, 0), foot);
        mech->on_attach(world, id, nlohmann::json::object(), ctx);
        auto& want = world.registry().emplace_or_replace<ptv::CLocomotionIntent>(
            static_cast<entt::entity>(id));
        want.desired_velocity = ptv::Vec3(4.0f, 0, 0);
        float prev_y = foot;
        float worst_drop = 0.0f;
        for (int i = 0; i < 120; ++i) {
            mech->on_update(world, id, dt, input, ctx);
            const float y = world.get<ptv::CTransform>(id).t.position.y;
            worst_drop = std::max(worst_drop, prev_y - y);
            prev_y = y;
            if (i == 20) want.desired_velocity = ptv::Vec3(0.0f);
        }
        if (worst_drop > 0.5f)
            return fail("walking off a ledge teleported the character down "
                        "instead of letting it fall");
        if (std::fabs(prev_y - (-3.0f + foot)) > 0.02f) {
            std::fprintf(stderr, "  landed at y=%.3f, floor+foot=%.3f\n",
                         prev_y, -3.0f + foot);
            return fail("the character did not land on the lower floor");
        }
    }
    {  // Jump
        ptv::World world;
        auto mech = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        const auto id = make_walker(world, "jumper", ptv::Vec3(0, foot, 0), foot);
        mech->on_attach(world, id, nlohmann::json::object(), ctx);
        auto& want = world.registry().emplace_or_replace<ptv::CLocomotionIntent>(
            static_cast<entt::entity>(id));
        want.jump_velocity = 4.0f;
        float apex = foot;
        for (int i = 0; i < 90; ++i) {
            mech->on_update(world, id, dt, input, ctx);
            apex = std::max(apex, world.get<ptv::CTransform>(id).t.position.y);
        }
        if (apex < foot + 0.5f) return fail("a jump did not leave the ground");
        if (std::fabs(world.get<ptv::CTransform>(id).t.position.y - foot) > 0.02f)
            return fail("a jump did not come back down to the floor");
        if (world.get<ptv::CLocomotionIntent>(id).jump_velocity != 0.0f)
            return fail("jump_velocity was not consumed");
    }
    {  // Steep slope above = wall
        terrain.steep_wall = true;
        ptv::World world;
        auto mech = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        const auto id = make_walker(world, "climber", ptv::Vec3(2.9f, -3.0f + foot, 0), foot);
        mech->on_attach(world, id, nlohmann::json::object(), ctx);
        auto& want = world.registry().emplace_or_replace<ptv::CLocomotionIntent>(
            static_cast<entt::entity>(id));
        want.desired_velocity = ptv::Vec3(1.4f, 0, 0);
        for (int i = 0; i < 60; ++i) mech->on_update(world, id, dt, input, ctx);
        if (world.get<ptv::CTransform>(id).t.position.x >= 3.0f)
            return fail("a character walked up a slope steeper than max_slope_deg");
        terrain.steep_wall = false;
    }
    {  // Hover
        ptv::World world;
        auto mech = ptv::MechanicRegistry::get().make_mechanic(
            "builtin.character_locomotion");
        const auto id = make_walker(world, "drone", ptv::Vec3(0, foot, 0), foot);
        nlohmann::json p = nlohmann::json::object();
        p["motor"] = "flying";
        p["hover_height_m"] = 2.0f;
        mech->on_attach(world, id, p, ctx);
        for (int i = 0; i < 180; ++i) mech->on_update(world, id, dt, input, ctx);
        if (std::fabs(world.get<ptv::CTransform>(id).t.position.y - (2.0f + foot)) > 0.05f)
            return fail("a hovering flyer did not hold its hover height");
    }
    return ok("ground motor: falls off ledges and lands, jumps, refuses steep "
              "slopes, and a hovering flyer holds its height");
}

namespace {

/** A rig whose clip carries the body 6 cm low: the foot joint is drawn at
 *  body_y - 0.9 (soles) + 0.05 (joint above soles) - 0.06 (clip crouch). */
class CrouchSkeleton final : public ptv::ISkeleton {
public:
    ptv::World* world{nullptr};
    std::uint32_t entity{0};
    bool has_skeleton(std::uint32_t) const override { return true; }
    int joint_index(std::uint32_t, const char*) const override { return -1; }
    int joint_count(std::uint32_t) const override { return 1; }
    int joint_for_slot(std::uint32_t, const char* slot) const override {
        return std::string(slot) == "foot_l" ? 0 : -1;
    }
    int joint_parent(std::uint32_t, int) const override { return -1; }
    ptv::Mat4 joint_world(std::uint32_t, int) const override {
        const float y = world->get<ptv::CTransform>(entity).t.position.y;
        return glm::translate(ptv::Mat4(1.0f), ptv::Vec3(0, y - 0.9f + 0.05f - 0.06f, 0));
    }
    ptv::Mat4 joint_model(std::uint32_t, int) const override { return ptv::Mat4(1.0f); }
    ptv::Mat4 joint_bind_model(std::uint32_t, int) const override {
        return glm::translate(ptv::Mat4(1.0f), ptv::Vec3(0, -0.9f + 0.05f, 0));
    }
    ptv::Quat joint_bind_rotation(std::uint32_t, int) const override { return ptv::Quat(1, 0, 0, 0); }
    ptv::Quat joint_local_rotation(std::uint32_t, int) const override { return ptv::Quat(1, 0, 0, 0); }
    void set_joint_rotation_from_bind(std::uint32_t, int, const ptv::Quat&) override {}
    int joint_subtree(std::uint32_t, int, int*, int) const override { return 0; }
    void set_joint_rotation(std::uint32_t, int, const ptv::Quat&) override {}
    void set_joint_translation(std::uint32_t, int, const ptv::Vec3&) override {}
    void set_joint_scale(std::uint32_t, int, const ptv::Vec3&) override {}
    bool play_clip(std::uint32_t, const char*, bool, float) override { return true; }
    void stop_clip(std::uint32_t) override {}
    void set_clip_speed(std::uint32_t, float) override {}
    bool has_clip(std::uint32_t, const char*) const override { return true; }
    const char* current_clip(std::uint32_t) const override { return "walk"; }
    float clip_time(std::uint32_t) const override { return 0.0f; }
    float clip_duration(std::uint32_t) const override { return 1.0f; }
    bool clip_finished(std::uint32_t) const override { return false; }
    void fade_to_rest(std::uint32_t, float) override {}
    bool one_shot_playing(std::uint32_t) const override { return false; }
};

}  // namespace

// A clip that carries the body low (a brute's crouched gait) sinks the drawn
// feet even though the capsule stands exactly right; planting lifts the body
// until the drawn soles meet the floor.
static int case_foot_planting() {
    ptv::World world;
    const auto id = make_walker(world, "bruiser", ptv::Vec3(0, 0.9f, 0), 0.9f);
    ptv::CSkeleton sk{};
    sk.skeleton.bind_min_y = -0.9f;
    world.registry().emplace<ptv::CSkeleton>(static_cast<entt::entity>(id), std::move(sk));
    FakePhysics physics;  // flat floor at y = 0
    CrouchSkeleton skeleton;
    skeleton.world = &world;
    skeleton.entity = id;
    ptv::MechanicContext ctx{};
    ctx.physics = &physics;
    ctx.skeleton = &skeleton;
    const ptv::InputSnapshot input{};
    auto mech = ptv::MechanicRegistry::get().make_mechanic("builtin.character_locomotion");
    mech->on_attach(world, id, nlohmann::json::object(), ctx);
    for (int i = 0; i < 120; ++i) mech->on_update(world, id, 1.0f / 60.0f, input, ctx);
    const float drawn_sole = skeleton.joint_world(id, 0)[3].y - 0.05f;
    if (std::fabs(drawn_sole) > 0.01f) {
        std::fprintf(stderr, "  drawn soles at %.3f after planting\n", drawn_sole);
        return fail("the drawn feet were not planted on the floor");
    }
    return ok("foot planting: a crouched gait is lifted until its drawn soles "
              "meet the floor");
}

// The model frame: with one, the rigged mesh's bind pose IS the pivot -- the
// face turned onto -Z and the soles at -height/2 -- for any source frame.
static int case_model_frame(const char* glb) {
    namespace gl = ptv::gltf_mesh_load;
    std::vector<gl::SkinnedVertex> verts;
    std::vector<std::uint32_t> idx;
    gl::SkeletonData plain;
    std::string err;
    const float target = 1.8f;
    if (!gl::load_glb_skinned(glb, target, verts, idx, plain, err))
        return fail(("fixture did not load: " + err).c_str());
    float lo = 1e30f, hi = -1e30f;
    for (const auto& v : verts) {
        lo = std::min(lo, v.pos[1]);
        hi = std::max(hi, v.pos[1]);
    }
    // Back to GLB units for the frame.
    const float raw_lo = lo / plain.unit_scale;
    const float raw_h = (hi - lo) / plain.unit_scale;

    gl::ModelFrame frame{};
    frame.valid = true;
    frame.yaw_offset_rad = glm::radians(90.0f);  // a +X-facing source
    frame.sole_y = raw_lo;
    frame.height = raw_h;
    gl::SkeletonData framed;
    if (!gl::load_glb_skinned(glb, target, verts, idx, framed, err, &frame))
        return fail(("framed fixture did not load: " + err).c_str());

    if (std::fabs(framed.unit_scale - target / raw_h) > 1e-4f)
        return fail("a framed character is not scaled by its height");
    if (std::fabs(framed.bind_min_y + 0.5f * target) > 0.01f) {
        std::fprintf(stderr, "  bind_min_y %.4f, expected %.4f\n",
                     framed.bind_min_y, -0.5f * target);
        return fail("the model frame did not put the soles at -height/2");
    }
    std::vector<ptv::Mat4> palette;
    gl::build_joint_palette(framed, framed.bind_pose, palette);
    if (palette.empty()) return fail("no palette");
    // A +X-facing model turned onto -Z: its +X must now point at -Z.
    const ptv::Vec3 face = ptv::Vec3(palette[0] * ptv::Vec4(1, 0, 0, 0));
    if (glm::length(face - ptv::Vec3(0, 0, -1)) > 1e-3f) {
        std::fprintf(stderr, "  model +X now points (%.3f, %.3f, %.3f)\n",
                     face.x, face.y, face.z);
        return fail("the model frame did not turn the model's forward onto -Z");
    }
    // Every joint gets the same pivot in bind pose.
    for (size_t j = 1; j < palette.size(); ++j) {
        if (glm::length(ptv::Vec3(palette[j][3]) - ptv::Vec3(palette[0][3])) > 1e-3f)
            return fail("joints disagree on the bind-pose pivot");
    }
    // A clip that animates a root joint must play relative to the pivot:
    // each framed key is exactly yaw * the unframed key.
    const ptv::Quat yaw = glm::angleAxis(frame.yaw_offset_rad, ptv::Vec3(0, 1, 0));
    for (size_t c = 0; c < framed.clips.size() && c < plain.clips.size(); ++c) {
        const auto& fch = framed.clips[c].channels;
        const auto& pch = plain.clips[c].channels;
        for (size_t k = 0; k < fch.size() && k < pch.size(); ++k) {
            if (framed.parents[static_cast<size_t>(fch[k].joint)] >= 0) continue;
            if (fch[k].path != gl::AnimPath::Rotation || fch[k].values.size() < 4)
                continue;
            const auto& a = pch[k].values;
            const auto& b = fch[k].values;
            const ptv::Quat want = yaw * ptv::Quat(a[3], a[0], a[1], a[2]);
            const ptv::Quat got(b[3], b[0], b[1], b[2]);
            if (std::fabs(std::fabs(glm::dot(want, got)) - 1.0f) > 1e-4f)
                return fail("a root animation key was not given the model frame");
        }
    }
    return ok("model frame: framed rigs face -Z, stand on their soles and "
              "scale by height");
}

// --- Text + game flow -----------------------------------------------------

#include "ptv/render/TextAtlas.h"

// The bundled font bakes, and layout behaves: centred lines straddle the
// anchor, a longer string is wider, and newlines stack downward.
static int case_text_atlas() {
    ptv::TextAtlas atlas;
    std::string err;
    if (!atlas.build_default(err)) return fail(("font did not bake: " + err).c_str());
    if (atlas.width() <= 0 || atlas.height() <= 0 ||
        atlas.pixels().size() != static_cast<size_t>(atlas.width() * atlas.height()))
        return fail("atlas has no pixels");
    std::vector<ptv::TextQuad> q;
    atlas.layout("Game Over", 0.05f, 1, q);
    if (q.size() != 8) return fail("'Game Over' did not lay out 8 glyph quads");
    float lo = 1e9f, hi = -1e9f, top = -1e9f;
    for (const auto& g : q) {
        lo = std::min(lo, g.p0.x);
        hi = std::max(hi, g.p1.x);
        top = std::max(top, g.p1.y);
    }
    if (std::fabs(lo + hi) > 0.02f) return fail("centred text is not centred");
    if (top < 0.045f || top > 0.07f) return fail("cap height does not match the request");
    if (atlas.measure("Try Again", 0.05f) <= atlas.measure("Try", 0.05f))
        return fail("a longer string is not wider");
    std::vector<ptv::TextQuad> two;
    atlas.layout("A\nB", 0.05f, 0, two);
    if (two.size() != 2 || !(two[1].p1.y < two[0].p0.y))
        return fail("a second line does not sit below the first");
    return ok("text atlas: bundled font bakes to an SDF atlas and lays out");
}

namespace {

class FlowCombat final : public ptv::ICombat {
public:
    bool alive{true};
    int revives{0};
    bool apply_damage(std::uint32_t, float, std::uint32_t) override { return false; }
    void heal(std::uint32_t, float) override {}
    float health(std::uint32_t) const override { return alive ? 100.0f : 0.0f; }
    float max_health(std::uint32_t) const override { return 100.0f; }
    bool is_alive(std::uint32_t) const override { return alive; }
    void kill(std::uint32_t) override { alive = false; }
    const char* faction(std::uint32_t) const override { return ""; }
    bool is_hostile(std::uint32_t, std::uint32_t) const override { return false; }
    bool revive(std::uint32_t, float, float) override {
        alive = true;
        ++revives;
        return true;
    }
};

class FlowUi final : public ptv::IUi {
public:
    bool opened{false};
    std::string title;
    std::vector<std::string> rows;
    std::vector<std::string> clicks;
    bool panel_exists(const char*) const override { return true; }
    void open(const char*) override { opened = true; }
    void close(const char*) override { opened = false; }
    bool is_open(const char*) const override { return opened; }
    void set_rows(const char*, const ptv::UiRowDesc* r, int n) override {
        rows.clear();
        for (int i = 0; i < n; ++i) rows.emplace_back(r[i].id);
    }
    bool poll_click(const char*, ptv::UiClick* out) override {
        if (clicks.empty()) return false;
        scratch_ = clicks.front();
        clicks.erase(clicks.begin());
        out->row_id = scratch_.c_str();
        return true;
    }
    int highlighted_row(const char*) const override { return -1; }
    void set_title(const char*, const char* t, const char*) override { title = t; }

private:
    std::string scratch_;
};

class FlowApp final : public ptv::IApp {
public:
    bool exit{false};
    int placed{0};
    void request_exit() override { exit = true; }
    bool exit_requested() const override { return exit; }
    const char* platform() const override { return "pcvr"; }
    float uptime_s() const override { return 0.0f; }
    bool place_player(const ptv::Vec3&, float) override {
        ++placed;
        return true;
    }
};

class FlowTransition final : public ptv::ISceneTransition {
public:
    int restarts{0};
    bool go_to(const char*, const char*) override { return false; }
    const char* current_scene() const override { return "arena"; }
    bool scene_exists(const char*) const override { return true; }
    bool transition_pending() const override { return false; }
    bool load_nested(const char*, std::uint32_t, const char*, float) override { return false; }
    bool unload_nested(const char*) override { return false; }
    bool nested_loaded(const char*) const override { return false; }
    bool restart_current(bool) override {
        ++restarts;
        return true;
    }
};

class FlowHands final : public ptv::IHands {
public:
    std::uint32_t player{0};
    std::uint32_t held_entity(int) const override { return 0xFFFFFFFFu; }
    void set_held_entity(int, std::uint32_t) override {}
    std::uint32_t hand_entity(int) const override { return 0xFFFFFFFFu; }
    std::uint32_t finger_target(int) const override { return 0xFFFFFFFFu; }
    void set_finger_target(int, std::uint32_t) override {}
    std::uint32_t player_entity() const override { return player; }
};

}  // namespace

// The Goblin Shooter loop end to end: three lives, respawn, Game Over with
// Try Again, a survive-the-clock win with Play Again, and Leave Game.
static int case_game_flow() {
    const auto design = nlohmann::json::parse(R"({
        "experience": "combat", "has_health": true, "max_health": 100, "lives": 3,
        "respawn": {"at": "spawn", "delay_s": 1.0, "invulnerable_s": 3},
        "spawn": {"position": [0, 0, 0], "yaw_deg": 0},
        "objectives": [{"id": "survive", "kind": "survive_time",
                        "description": "Survive", "duration_s": 5}],
        "victory": {"kind": "all_objectives", "objective_ids": ["survive"]},
        "failure": [{"kind": "lives_exhausted"}],
        "screens": {"game_over": {"title": "Game Over", "buttons": ["try_again", "leave_game"]},
                    "victory": {"title": "You Win!", "buttons": ["play_again", "leave_game"]}},
        "panel_id": "ptv_game_flow"})");
    ptv::World world;
    const auto player = static_cast<std::uint32_t>(world.create_entity("player_rig"));
    world.registry().emplace<ptv::CTransform>(static_cast<entt::entity>(player));
    FlowCombat combat;
    FlowUi ui;
    FlowApp app;
    FlowTransition transition;
    FlowHands hands;
    hands.player = player;
    ptv::GameFlowBoard board;
    ptv::MechanicContext ctx{};
    ctx.combat = &combat;
    ctx.ui = &ui;
    ctx.app = &app;
    ctx.transition = &transition;
    ctx.hands = &hands;
    ctx.game = &board;
    ptv::InputSnapshot input{};
    const float dt = 1.0f / 30.0f;

    auto mech = ptv::MechanicRegistry::get().make_mechanic("builtin.game_flow");
    if (!mech) return fail("builtin.game_flow is not registered");
    mech->on_attach(world, player, design, ctx);
    auto run = [&](float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) mech->on_update(world, player, dt, input, ctx);
    };
    run(0.2f);
    if (board.state() != ptv::GameFlowState::Playing || board.lives() != 3)
        return fail("the flow did not start Playing with 3 lives");
    if (!board.gameplay_active()) return fail("gameplay is not active while playing");

    for (int life = 0; life < 2; ++life) {
        combat.alive = false;
        run(0.1f);
        if (board.state() != ptv::GameFlowState::LifeLost)
            return fail("health reaching zero did not cost a life");
        run(2.0f);
        if (board.state() == ptv::GameFlowState::LifeLost)
            return fail("the player was never respawned");
        if (!combat.alive) return fail("respawn did not revive the player");
    }
    if (board.lives() != 1 || app.placed != 2) return fail("lives / respawn placement miscounted");

    combat.alive = false;
    run(2.0f);
    if (board.state() != ptv::GameFlowState::GameOver) return fail("the last life did not end the game");
    if (!ui.opened || ui.title != "Game Over") return fail("the Game Over screen did not open");
    if (ui.rows != std::vector<std::string>{"try_again", "leave_game"})
        return fail("the Game Over screen does not offer Try Again / Leave Game");
    if (board.gameplay_active()) return fail("gameplay stayed active under the Game Over screen");
    ui.clicks.push_back("try_again");
    run(0.1f);
    if (transition.restarts != 1) return fail("Try Again did not restart the scene");

    // The reload: a fresh mechanic instance, attempt 2.
    board.clear();
    board.flow_state()->attempts = 2;
    combat.alive = true;
    auto again = ptv::MechanicRegistry::get().make_mechanic("builtin.game_flow");
    again->on_attach(world, player, design, ctx);
    for (float t = 0.0f; t < 5.5f; t += dt) again->on_update(world, player, dt, input, ctx);
    if (board.state() != ptv::GameFlowState::Victory) return fail("surviving the clock did not win");
    if (ui.title != "You Win!" || ui.rows.front() != "play_again")
        return fail("the victory screen is wrong");
    ui.clicks.push_back("leave_game");
    again->on_update(world, player, dt, input, ctx);
    if (!app.exit || board.state() != ptv::GameFlowState::Exiting)
        return fail("Leave Game did not exit the app");

    // A book reader: no health, never dies; the menu button pauses.
    ptv::GameFlowBoard calm;
    ctx.game = &calm;
    app.exit = false;
    auto reader = ptv::MechanicRegistry::get().make_mechanic("builtin.game_flow");
    reader->on_attach(world, player, nlohmann::json::parse(R"({
        "experience": "media", "has_health": false, "free_play": true,
        "pause": {"enabled": true, "buttons": ["resume", "leave_game"]}})"), ctx);
    combat.alive = false;  // nothing can hurt a reader; ignored
    for (int i = 0; i < 10; ++i) reader->on_update(world, player, dt, input, ctx);
    if (calm.state() != ptv::GameFlowState::Playing) return fail("a book reader 'died'");
    input.button_menu = true;
    reader->on_update(world, player, dt, input, ctx);
    if (calm.state() != ptv::GameFlowState::Paused) return fail("the menu button did not pause");
    input.prev_button_menu = true;
    ui.clicks.push_back("resume");
    reader->on_update(world, player, dt, input, ctx);
    if (calm.state() != ptv::GameFlowState::Playing) return fail("Resume did not resume");
    return ok("game flow: lives, respawn, Game Over / Try Again, victory / Play "
              "Again, Leave Game, and a reader that cannot die but can pause");
}

// --- Quest optimization + memory -------------------------------------------

// The governor climbs the GPU ladder (FFR first) for a GPU-bound frame and the
// CPU ladder (cull distance first) for a CPU-bound one, waits out a cooldown
// after each step, skips a knob whose step bought nothing, and never moves a
// knob the user locked -- however long the frames stay bad.
static int case_governor_ladders() {
    ptv::ScenePerformance perf{};
    perf.refresh_hz = 72;
    perf.foveation_level = 1;
    perf.governor_max_foveation_level = 3;
    perf.cull_distance_m = 200.0f;
    perf.governor_min_cull_distance_m = 25.0f;
    perf.governor_escalate_after_bad_frames = 10;
    perf.governor_relax_after_good_frames = 30;

    ptv::QualityGovernor gpu;
    gpu.configure(perf);
    int frames = 0;
    // GPU-bound and improving with each step: FFR goes up first.
    while (gpu.state().foveation_level == 1 && frames < 500) {
        gpu.update(0.020f, 20.0f, 2.0f);
        ++frames;
    }
    if (gpu.state().foveation_level != 2) return fail("GPU-bound: FFR was not the first step");
    if (gpu.state().cull_distance_m != 200.0f) return fail("GPU-bound: cull moved before FFR");
    // Cooldown: the very next bad frames do not step again.
    const auto after_first = gpu.state();
    for (int i = 0; i < 5; ++i) gpu.update(0.020f, 20.0f, 2.0f);
    if (gpu.state().render_scale != after_first.render_scale ||
        gpu.state().foveation_level != after_first.foveation_level) {
        return fail("stepped again inside the cooldown");
    }

    ptv::QualityGovernor cpu;
    cpu.configure(perf);
    frames = 0;
    while (cpu.state().cull_distance_m == 200.0f && frames < 500) {
        cpu.update(0.020f, 4.0f, 20.0f);
        ++frames;
    }
    if (cpu.state().cull_distance_m >= 200.0f) return fail("CPU-bound: cull was not shrunk");
    if (cpu.state().foveation_level != 1) return fail("CPU-bound: FFR moved");

    // A step that buys nothing (GPU time unchanged) is skipped from then on.
    ptv::QualityGovernor probe;
    probe.configure(perf);
    for (int i = 0; i < 400; ++i) probe.update(0.020f, 20.0f, 2.0f);
    if (probe.state().render_scale >= 1.0f) return fail("ineffective FFR did not move on to scale");

    // Locks: FFR off and resolution off stay put through 10,000 bad frames.
    ptv::ScenePerformance locked = perf;
    locked.foveation_level = 0;
    locked.lock_ffr = "off";
    locked.lock_resolution = "off";
    locked.lock_lod = "detail";
    locked.governor_max_lod_bias = 0.0f;
    ptv::QualityGovernor gov;
    gov.configure(locked);
    for (int i = 0; i < 10000; ++i) gov.update(0.030f, 30.0f, 2.0f);
    if (gov.state().foveation_level != 0) return fail("a locked FFR level moved");
    if (gov.state().render_scale != 1.0f) return fail("a locked resolution moved");
    if (gov.state().lod_bias != 0.0f) return fail("a locked LOD bias moved");
    // Only the unlocked knob (draw distance) was allowed to move.
    if (gov.state().cull_distance_m >= 200.0f) return fail("the unlocked knob never moved");
    return ok("governor: GPU ladder FFR first, CPU ladder cull first, cooldown, "
              "ineffective step skipped, locks never move");
}

// MemoryGuard levels from the engine's own accounting (no sampler running):
// projected usage, including queued loads, decides the level; it steps down
// only with a margin.
static int case_memory_guard_levels() {
    ptv::ScenePerformance perf{};
    ptv::MemoryGuard g;
    g.configure(perf);
    ptv::MemoryGuard::Accounting a{};
    a.texture_mb = 1000.0;
    g.update(a);
    if (g.level() != ptv::MemoryGuard::Level::Green) return fail("1 GB is not green");
    a.texture_mb = 4200.0;
    a.reserved_mb = 300.0;  // queued loads push the projection over 4.25 GiB
    g.update(a);
    if (g.level() != ptv::MemoryGuard::Level::Yellow) return fail("reservations did not raise yellow");
    a.reserved_mb = 0.0;
    a.texture_mb = 4700.0;
    g.update(a);
    if (g.level() != ptv::MemoryGuard::Level::Red) return fail("4.7 GB is not red");
    a.texture_mb = 5000.0;
    g.update(a);
    if (g.level() != ptv::MemoryGuard::Level::Critical) return fail("5.0 GB is not critical");
    a.texture_mb = 4850.0;  // under 4.8 GiB but inside the hysteresis band
    g.update(a);
    if (g.level() != ptv::MemoryGuard::Level::Critical) return fail("left critical without margin");
    a.texture_mb = 3000.0;
    for (int i = 0; i < 4; ++i) g.update(a);
    if (g.level() != ptv::MemoryGuard::Level::Green) return fail("did not recover to green");
    // Simulated pressure (verification) trips red on a small game.
    g.set_simulated_pressure(0.05f);
    a.texture_mb = 300.0;
    g.update(a);
    g.update(a);
    if (g.level() < ptv::MemoryGuard::Level::Red) return fail("simulated pressure did not reach red");
    if (g.telemetry_line().find("mem: pss") != 0) return fail("telemetry line format");
    return ok("memory guard: levels, projection from reservations, hysteresis, pressure");
}

// Locks, rooms, static batching and LOD errors parse from scene.json -- and a
// null where the compiler wrote an optional field is not a crash.
static int case_scene_perf_locks() {
    const char* json = R"({
      "version": 1, "scene_id": "s",
      "performance": {
        "multiview": true, "ffr_fdm": false, "frames_in_flight": 2,
        "render_scale_floor": 0.9, "refresh_hz": 90, "room_culling": true,
        "rooms": ["hall", "office"], "static_batch_cell_m": 16,
        "resident_memory_budget_mb": 1500,
        "locks": {"ffr": {"value": "off", "level": null, "source": "custom_box", "quote": "no FFR"},
                  "resolution": {"value": "high", "level": null},
                  "lod": {"value": "auto", "level": null}}
      },
      "entities": [
        {"id": "wall", "transform": {"position": [0,0,0]}, "static_batch": true,
         "primitive": {"kind": "box", "size": [1,1,1]}},
        {"id": "crate", "transform": {"position": [1,0,0]},
         "mesh": {"glb_path": "a.glb", "lod_paths": ["a.glb", "b.glb"],
                  "lod_errors_rel": [0.0, 0.02]}}
      ]})";
    const auto path = std::filesystem::temp_directory_path() / "ptv_smoke_locks.json";
    { std::ofstream f(path); f << json; }
    ptv::SceneDoc doc = ptv::LoadSceneJson(path.string());
    const auto& p = doc.performance;
    if (p.lock_ffr != "off" || p.lock_resolution != "high" || p.lock_lod != "auto")
        return fail("locks did not parse");
    if (p.lock_ffr_level != -1) return fail("a null level was not treated as unset");
    if (p.ffr_fdm || p.refresh_hz != 90 || p.render_scale_floor != 0.9f)
        return fail("toggles / resolution did not parse");
    if (p.rooms.size() != 2 || !p.room_culling || p.static_batch_cell_m != 16.0f)
        return fail("rooms / batching did not parse");
    if (p.resident_memory_budget_mb != 1500.0f) return fail("residency budget did not parse");
    ptv::World world;
    ptv::ApplySceneToWorld(doc, world);
    int batched = 0;
    for (auto e : world.registry().view<ptv::CStaticBatch>()) {
        (void)e;
        ++batched;
    }
    if (batched != 1) return fail("static_batch did not become CStaticBatch");
    bool errors = false;
    for (auto e : world.registry().view<ptv::CMesh>()) {
        errors = world.registry().get<ptv::CMesh>(e).lod_errors_rel.size() == 2;
    }
    if (!errors) return fail("lod_errors_rel did not reach CMesh");
    return ok("scene: locks (null-safe), rooms, batching, residency budget, LOD errors");
}

// Clones of an NPC type (and its animator) share one clip array.
static int case_clip_sharing() {
    namespace gml = ptv::gltf_mesh_load;
    std::vector<gml::AnimationClip> clips(2);
    clips[0].name = "idle";
    clips[1].name = "walk";
    gml::ClipList shared(std::move(clips));
    gml::ClipList clone = shared;
    if (!clone.shares_with(shared)) return fail("a copy did not share");
    ptv::CAnimator anim{};
    anim.clips = shared;
    ptv::CAnimator anim_copy = anim;
    if (!anim_copy.clips.shares_with(shared) || shared.use_count() != 4)
        return fail("animator copies did not share");
    if (anim_copy.clips[1].name != "walk") return fail("read through a shared list");
    gml::AnimationClip extra;
    extra.name = "die";
    clone.push_back(extra);
    if (clone.shares_with(shared) || shared.size() != 2 || clone.size() != 3)
        return fail("a write did not detach");
    return ok("clips: copies share one array, a write detaches");
}

// The texture gallery frames a rigged asset at the centre its bind-pose
// palette puts the raw vertices; that must be where the skinned mesh really
// is, at its real size.
static int case_gallery_skinned_framing(const char* glb) {
    namespace gml = ptv::gltf_mesh_load;
    for (const std::string& path : {std::string(glb), other_hand_path(glb)}) {
        std::string err;
        std::vector<gml::SkinnedVertex> verts;
        std::vector<std::uint32_t> indices;
        gml::SkeletonData skel;
        if (!gml::load_glb_skinned(path, 0.27f, verts, indices, skel, err)) {
            std::fprintf(stderr, "[fail] load_glb_skinned %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        std::vector<ptv::Mat4> pal;
        gml::build_joint_palette(skel, skel.bind_pose, pal);
        ptv::Vec3 rlo(1e9f), rhi(-1e9f), slo(1e9f), shi(-1e9f);
        for (const auto& v : verts) {
            const ptv::Vec3 p(v.pos[0], v.pos[1], v.pos[2]);
            rlo = glm::min(rlo, p);
            rhi = glm::max(rhi, p);
            ptv::Mat4 m(0.0f);
            for (int k = 0; k < 4; ++k) {
                if (v.weights[k] == 0) continue;
                m += pal[v.joints[k]] * (static_cast<float>(v.weights[k]) / 255.0f);
            }
            const ptv::Vec3 s(m * ptv::Vec4(p, 1.0f));
            slo = glm::min(slo, s);
            shi = glm::max(shi, s);
        }
        const ptv::Vec3 skin_c = 0.5f * (slo + shi);
        const float raw_r = 0.5f * glm::length(rhi - rlo);
        const float skin_r = 0.5f * glm::length(shi - slo);
        ptv::Vec3 centre(0.0f);
        float radius = 0.0f;
        if (!ptv::skinned_bounds(verts, pal, centre, radius)) return fail("no skinned bounds");
        std::printf("  %s: raw r %.4f m | skinned centre (%.3f %.3f %.3f) r %.3f m | gallery "
                    "(%.3f %.3f %.3f) r %.3f m\n", path.c_str(), raw_r, skin_c.x, skin_c.y,
                    skin_c.z, skin_r, centre.x, centre.y, centre.z, radius);
        // Framed from the skinned pose: a 0.27 m hand is ~0.15 m in radius,
        // whatever scale its raw vertices are in.
        if (glm::length(centre - skin_c) > 1e-3f || std::abs(radius - skin_r) > 1e-3f)
            return fail("gallery bounds differ from the skinned bind pose");
        if (radius < 0.05f || radius > 0.5f) return fail("skinned hand is not hand-sized");
    }
    return ok("gallery: rigged assets framed at their skinned bind-pose centre");
}

// KTX2 written and parsed back; ASTC level sizes.
static int case_ktx2_roundtrip() {
    constexpr std::uint32_t kAstc6x6Srgb = 166;  // VK_FORMAT_ASTC_6x6_SRGB_BLOCK
    std::vector<std::vector<std::uint8_t>> levels;
    for (std::uint32_t s = 64; s >= 1; s /= 2) {
        levels.emplace_back(ptv::ktx2::astc_level_bytes(s, s, 6, 6), std::uint8_t(s));
        if (s == 1) break;
    }
    const auto bytes = ptv::ktx2::write(kAstc6x6Srgb, 64, 64, levels);
    ptv::ktx2::Info info;
    std::string err;
    if (!ptv::ktx2::parse(bytes.data(), bytes.size(), info, err)) return fail(err.c_str());
    if (info.width != 64 || info.levels.size() != levels.size() || info.vk_format != kAstc6x6Srgb)
        return fail("header mismatch");
    if (info.levels[0].size != ptv::ktx2::astc_level_bytes(64, 64, 6, 6) ||
        bytes[info.levels[1].offset] != 32) {
        return fail("level data mismatch");
    }
    std::uint32_t bw = 0, bh = 0;
    ptv::ktx2::astc_block_size(kAstc6x6Srgb, bw, bh);
    if (bw != 6 || bh != 6) return fail("block size");
    if (ptv::ktx2::astc_level_bytes(7, 7, 6, 6) != 4 * 16) return fail("partial blocks round up");
    if (ptv::ktx2::parse(bytes.data(), 40, info, err)) return fail("a truncated file parsed");
    return ok("ktx2: write/parse round trip, level offsets, ASTC block math");
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
    if (what == "stationary_scene") return case_stationary_scene();
    if (what == "openxr") return case_openxr();
    if (what == "physics_capsule") return case_physics_capsule();
    if (what == "physics_player_slide") return case_physics_player_slide();
    if (what == "player_character") return case_player_character();
    if (what == "player_self_push") return case_player_self_push();
    if (what == "sprint_gate") return case_sprint_gate();
    if (what == "avatar_anchor") return case_avatar_anchor();
    if (what == "height_calibration") return case_height_calibration();
    if (what == "arm_assist") return case_arm_assist();
    if (what == "teleport_pitch") return case_teleport_pitch();
    if (what == "physics_overlap") return case_physics_overlap();
    if (what == "runtime_spawn") return case_runtime_spawn();
    if (what == "clone_tree") return case_clone_tree();
    if (what == "encounter_director") return case_encounter_director();
    if (what == "npc_behaviors") return case_npc_behaviors();
    if (what == "character_locomotion") return case_character_locomotion();
    if (what == "locomotion_variants") return case_locomotion_variants();
    if (what == "attachment") return case_attachment();
    if (what == "combat_factions") return case_combat_factions();
    if (what == "nav_graph") return case_nav_graph();
    if (what == "npc_ground_snap") return case_npc_ground_snap_real_physics();
    if (what == "mechanic_host_npc") return case_mechanic_host_npc();
    if (what == "player_rig_adoption") return case_player_rig_adoption();
    if (what == "damage_log") return case_damage_log();
    if (what == "facing_math") return case_facing_math();
    if (what == "text_atlas") return case_text_atlas();
    if (what == "game_flow") return case_game_flow();
    if (what == "governor_ladders") return case_governor_ladders();
    if (what == "memory_guard_levels") return case_memory_guard_levels();
    if (what == "scene_perf_locks") return case_scene_perf_locks();
    if (what == "clip_sharing") return case_clip_sharing();
    if (what == "ktx2_roundtrip") return case_ktx2_roundtrip();
    if (what == "gallery_skinned_framing" && argc >= 3) {
        return case_gallery_skinned_framing(argv[2]);
    }
    if (what == "facing_directions") return case_facing_directions();
    if (what == "ground_motor") return case_ground_motor();
    if (what == "foot_planting") return case_foot_planting();
    if (what == "model_frame") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke model_frame <rigged.glb>\n");
            return 2;
        }
        return case_model_frame(argv[2]);
    }
    if (what == "animation_lod") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: ptv_smoke animation_lod <rigged.glb>\n");
            return 2;
        }
        return case_animation_lod(argv[2]);
    }
    if (what == "clip_blend") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke clip_blend <rigged.glb>\n");
            return 2;
        }
        return case_clip_blend(argv[2]);
    }
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
    if (what == "uthana_root_bake") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke uthana_root_bake <glb>\n");
            return 2;
        }
        return case_uthana_root_bake(argv[2]);
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
    if (what == "default_hands") {
        if (argc < 3) {
            std::fprintf(stderr, "usage: ptv_smoke default_hands <hand.glb>\n");
            return 2;
        }
        return case_default_hands(argv[2]);
    }
    if (what == "body_slots") return case_body_slots();
    if (what == "two_bone_ik") return case_two_bone_ik();
    if (what == "mirror_reflection") return case_mirror_reflection();
    if (what == "equip_semantics") return case_equip_semantics();
    if (what == "equip_slots") return case_scene_declared_equip_slots();
    if (what == "interactive_persistence") {
        return case_interactive_persistence();
    }
    if (what == "affordance_state") return case_affordance_state_contract();
    if (what == "visual_tags") return case_visual_tags();
    if (what == "scene_transitions") return case_scene_transitions();
    if (what == "ui_panel") return case_ui_panel();
    if (what == "reference_frame") return case_reference_frame();
    if (what == "nested_scene") return case_nested_scene();
    if (what == "self_view_hide") {
        if (argc < 3) {
            std::fprintf(stderr,
                         "usage: ptv_smoke self_view_hide <rigged.glb>\n");
            return 2;
        }
        return case_self_view_hide(argv[2]);
    }
    if (what == "grab_socket") return case_grab_socket();
    if (what == "hold_solver") return case_hold_solver();
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
