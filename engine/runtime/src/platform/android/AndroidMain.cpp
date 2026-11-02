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
#include <unistd.h>

#include <memory>

#include "AndroidAssetSource.h"
#include "AndroidPlatform.h"
#include "ptv/core/Application.h"
#include "ptv/core/Logger.h"
#include "ptv/core/MemoryGuard.h"
#include "ptv/platform/AssetSource.h"

namespace {
// NativeActivity lifecycle callback. On Quest the actual VR frame loop and its
// gating on visibility/focus are driven by the OpenXR session state machine
// (XrRuntime::poll_events); native_app_glue also updates android_app fields
// (window, destroyRequested) that the Application loop reads. We log the key
// transitions here since on-device bring-up (surface/focus timing, controller
// re-acquire on resume) is the main thing that needs iteration on hardware.
void handle_app_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            PTV_INFO("android: APP_CMD_INIT_WINDOW (native window ready)");
            break;
        case APP_CMD_TERM_WINDOW:
            PTV_INFO("android: APP_CMD_TERM_WINDOW (native window destroyed)");
            break;
        case APP_CMD_GAINED_FOCUS:
            PTV_INFO("android: APP_CMD_GAINED_FOCUS");
            break;
        case APP_CMD_LOST_FOCUS:
            PTV_INFO("android: APP_CMD_LOST_FOCUS");
            break;
        case APP_CMD_RESUME:
            PTV_INFO("android: APP_CMD_RESUME");
            break;
        case APP_CMD_PAUSE:
            PTV_INFO("android: APP_CMD_PAUSE");
            break;
        case APP_CMD_DESTROY:
            PTV_INFO("android: APP_CMD_DESTROY (activity tearing down)");
            break;
        case APP_CMD_LOW_MEMORY:
            // A late, extra signal: MemoryGuard normally acts well before it.
            ptv::MemoryGuard::notify_low_memory();
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

    // The frame loop ended on its own (Leave Game, or the XR session
    // exiting) rather than because Android destroyed the activity. Returning
    // now would leave the process in a state the Quest cannot relaunch, so
    // ask the framework to finish the activity and wait for it to say so
    // (KhronosGroup/OpenXR-SDK-Source#329; ANativeActivity_finish).
    if (!app->destroyRequested) {
        ANativeActivity_finish(app->activity);
        while (!app->destroyRequested) {
            int events = 0;
            android_poll_source* source = nullptr;
            if (ALooper_pollOnce(100, nullptr, &events,
                                 reinterpret_cast<void**>(&source)) >= 0 &&
                source != nullptr) {
                source->process(app, source);
            }
        }
        PTV_INFO("android_main: activity finished");
        // Android keeps a finished activity's process cached, and the next
        // launch would reuse it -- with every engine singleton (asset source,
        // registries, logger) still holding the previous run. Leave Game
        // means the game is over: end the process so the next launch is a
        // clean one. _exit, not exit: static destructors racing the Java
        // side's teardown is exactly what must not happen here.
        _exit(0);
    }
}

#endif  // __ANDROID__
