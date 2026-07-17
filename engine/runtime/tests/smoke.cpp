// Lightweight C++ smoke tests for the runtime.
//
// Usage: `ptv_smoke <case>` where <case> is one of:
//    mechanics   — registry knows the built-ins
//    scene       — parses a trivial inline scene
//    openxr      — tries to create an XrInstance; passes even if no runtime

#include <cstdio>
#include <cstring>
#include <string>

#include "ptv/ecs/World.h"
#include "ptv/mechanics/MechanicRegistry.h"
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
        "builtin.smooth_locomotion",
        "builtin.locomotion.slide.head_relative",
        "builtin.locomotion.slide.controller_relative",
        "builtin.snap_turn",
        "builtin.locomotion.snap_turn",
        "builtin.grab",
        "builtin.throw",
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
                    "target_scale_meters": 0.2, "collision_shape": "aabb"},
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
    ptv::World world;
    ptv::ApplySceneToWorld(loaded, world);
    return ok("scene: 1 entity applied into ECS");
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
        std::fprintf(stderr, "usage: ptv_smoke <mechanics|scene|openxr>\n");
        return 2;
    }
    std::string what = argv[1];
    if (what == "mechanics") return case_mechanics();
    if (what == "scene") return case_scene();
    if (what == "openxr") return case_openxr();
    std::fprintf(stderr, "unknown case: %s\n", argv[1]);
    return 2;
}
