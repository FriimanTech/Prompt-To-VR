// Quest / Android native entry point.
//
// This is the standalone-headset counterpart to samples/starter-room/main.cpp.
// Instead of an int main() + GLFW mirror window, a Quest build is a
// NativeActivity: the Android framework calls android_main() on a dedicated
// thread via android_native_app_glue. We:
//   1. store the android_app so XrSession can initialise the Android OpenXR
//      loader (needs the JavaVM + activity context),
//   2. install an AAssetManager-backed asset source so scene.json / GLBs /
//      .spv shaders load from inside the APK,
//   3. hand control to ptv::Application::run(), which on Android pumps the
//      NativeActivity event queue instead of GLFW.
//
// NOTE: native bring-up (surface/focus timing, controller re-acquire on
// resume) requires on-device iteration; this file is the integration seam.
#if defined(__ANDROID__)

#include <android_native_app_glue.h>

#include <memory>

#include "AndroidAssetSource.h"
#include "AndroidPlatform.h"
#include "ptv/core/Application.h"
#include "ptv/core/Logger.h"
#include "ptv/platform/AssetSource.h"

namespace {
bool g_destroy_requested = false;

void handle_app_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_DESTROY:
            g_destroy_requested = true;
            break;
        default:
            break;
    }
    (void)app;
}
}  // namespace

// android_native_app_glue calls this on the app's main thread.
extern "C" void android_main(android_app* app) {
    app->onAppCmd = handle_app_cmd;

    ptv::set_android_app(app);
    ptv::set_asset_source(
        std::make_unique<ptv::AndroidAssetSource>(app->activity->assetManager));

    PTV_INFO("android_main: starting Prompt-To-VR Quest runtime");

    ptv::Application::Config cfg;
    cfg.app_name = "ptv_quest";
    // On Quest the game's scene ships inside the APK assets/.
    cfg.scene_json_path = "scene.json";
    cfg.enable_validation = false;  // validation layers are not shipped in the APK

    ptv::Application appn;
    const int rc = appn.run(cfg);
    PTV_INFO("android_main: runtime exited rc={}", rc);
    (void)g_destroy_requested;
}

#endif  // __ANDROID__
