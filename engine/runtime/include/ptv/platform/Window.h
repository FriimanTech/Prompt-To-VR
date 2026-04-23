#pragma once

#include <string>

struct GLFWwindow;

namespace ptv {

/**
 * GLFW desktop mirror window.
 *
 * In an OpenXR application the HMD is driven by the XR runtime, not by the
 * desktop surface. The mirror window exists to:
 *   * show a preview of what the player sees,
 *   * host Dear ImGui debug panels,
 *   * keep the application responsive to keyboard shortcuts.
 */
class Window {
public:
    Window(int width, int height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool should_close() const;
    void poll_events() const;
    void swap_buffers() const;

    GLFWwindow* handle() const { return window_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    GLFWwindow* window_{nullptr};
    int width_{1280};
    int height_{720};
};

}  // namespace ptv
