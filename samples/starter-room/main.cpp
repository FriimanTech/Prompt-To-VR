// Hand-authored starter-room sample. Boots the runtime with no LLM calls so
// you can validate Vulkan + OpenXR + mechanics before spending API credits.

#include <cstdio>
#include <filesystem>
#include <string>

#include "ptv/core/Application.h"

int main(int argc, char** argv) {
    ptv::Application::Config cfg;
    cfg.app_name = "ptv_starter_room";
    cfg.mirror_width = 1280;
    cfg.mirror_height = 720;

    // argv[1] is the explicit top-priority scene path. The orchestrator's
    // "Launch latest build" passes the generated project's scene.json here.
    if (argc >= 2) {
        cfg.scene_json_path = argv[1];
    }

    // Sidecar scene.json next to the executable (copied by CMake POST_BUILD
    // for the starter-room demo). Only consulted when neither argv[1] nor
    // PTV_SCENE_JSON is set -- otherwise double-clicking the exe would
    // override a launcher-specified project scene.
    namespace fs = std::filesystem;
    fs::path exe = fs::absolute(argv[0]);
    fs::path sidecar = exe.parent_path() / "scene.json";
    if (fs::exists(sidecar)) {
        cfg.scene_json_fallback_path = sidecar.string();
    }

    ptv::Application app;
    return app.run(cfg);
}
