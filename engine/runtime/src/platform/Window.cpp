#include "ptv/platform/Window.h"

#include <GLFW/glfw3.h>
#include <stdexcept>

#include "ptv/core/Logger.h"

namespace ptv {

static int g_glfw_users = 0;

Window::Window(int width, int height, const std::string& title)
    : width_(width), height_(height) {
    if (g_glfw_users == 0) {
        if (!glfwInit()) {
            throw std::runtime_error("glfwInit failed");
        }
    }
    ++g_glfw_users;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        PTV_ERROR("glfwCreateWindow failed; is a display attached?");
        --g_glfw_users;
        if (g_glfw_users == 0) glfwTerminate();
        throw std::runtime_error("glfwCreateWindow failed");
    }
    PTV_INFO("Mirror window created {}x{}", width, height);
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    --g_glfw_users;
    if (g_glfw_users == 0) {
        glfwTerminate();
    }
}

bool Window::should_close() const {
    return window_ ? glfwWindowShouldClose(window_) : true;
}

void Window::poll_events() const {
    glfwPollEvents();
}

void Window::swap_buffers() const {
    // No OpenGL context; the mirror surface is Vulkan-presented elsewhere.
}

}  // namespace ptv
