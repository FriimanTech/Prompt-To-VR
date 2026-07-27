// Implementation of the small process-global Android app handoff declared in
// AndroidPlatform.h. ``android_main`` (AndroidMain.cpp) stores the
// ``android_app*`` here once at startup; the engine's Android code paths
// (XrSession loader init, the Application NativeActivity event pump) read it
// back via ``current_android_app()``. Android-only.
#if defined(__ANDROID__)

#include "AndroidPlatform.h"

namespace ptv {
namespace {
// Set once from the NativeActivity thread before the engine starts and only
// read afterwards, so a plain pointer is sufficient (no synchronisation).
android_app* g_android_app = nullptr;
}  // namespace

void set_android_app(android_app* app) { g_android_app = app; }

android_app* current_android_app() { return g_android_app; }

}  // namespace ptv

#endif  // __ANDROID__
