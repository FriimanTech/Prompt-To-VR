#pragma once

// Small process-global handoff between the Android NativeActivity entry point
// (AndroidMain.cpp) and the parts of the engine that need the Android app /
// JavaVM (XrSession loader init, the Application event pump). Android-only.
#if defined(__ANDROID__)

struct android_app;

namespace ptv {

/** Set once by ``android_main`` before the engine starts. */
void set_android_app(android_app* app);

/** The current NativeActivity app, or nullptr before init. */
android_app* current_android_app();

}  // namespace ptv

#endif  // __ANDROID__
