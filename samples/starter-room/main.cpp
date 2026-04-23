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
    if (argc >= 2) {
        cfg.scene_json_path = argv[1];
    } else {
        // Resolve scene.json relative to the executable.
        namespace fs = std::filesystem;
        fs::path exe = fs::absolute(argv[0]);
        fs::path sidecar = exe.parent_path() / "scene.json";
        if (fs::exists(sidecar)) {
            cfg.scene_json_path = sidecar.string();
        }
    }
    ptv::Application app;
    return app.run(cfg);
}
