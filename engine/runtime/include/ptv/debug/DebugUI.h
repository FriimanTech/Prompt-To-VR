#pragma once

// Forward-declare at global scope so this resolves to the real GLFWwindow
// defined by <GLFW/glfw3.h>, not a namespace-local struct.
struct GLFWwindow;

namespace ptv {

class World;
struct XrFrameInfo {
    double frame_ms{0.0};
    int width{0};
    int height{0};
};

/** Tiny Dear ImGui overlay on the mirror window (optional).
 *
 * Only the developer / debug build should call ``draw``. In release the
 * overlay is compiled out via the CMake ``PTV_ENABLE_DEBUG_UI`` option.
 */
class DebugUI {
public:
    static bool init(::GLFWwindow* window);
    static void shutdown();

    static void new_frame();
    static void draw(World& world, const XrFrameInfo& info);
    static void render_to_mirror_window();
};

}  // namespace ptv
