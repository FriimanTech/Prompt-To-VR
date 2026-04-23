#include "ptv/debug/DebugUI.h"

#include <imgui.h>
// vcpkg's imgui[glfw-binding] feature installs the backend headers at the
// include root (not under backends/).
#include <imgui_impl_glfw.h>

#include "ptv/core/Logger.h"
#include "ptv/ecs/World.h"

namespace ptv {

namespace {
bool g_ready = false;
}

bool DebugUI::init(::GLFWwindow* window) {
    if (g_ready) return true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForOther(window, /*install_callbacks=*/true)) {
        return false;
    }
    g_ready = true;
    PTV_INFO("Dear ImGui initialized");
    return true;
}

void DebugUI::shutdown() {
    if (!g_ready) return;
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

void DebugUI::new_frame() {
    if (!g_ready) return;
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void DebugUI::draw(World& world, const XrFrameInfo& info) {
    if (!g_ready) return;
    ImGui::Begin("ptv debug");
    ImGui::Text("Mirror: %dx%d", info.width, info.height);
    ImGui::Text("Frame: %.2f ms", info.frame_ms);
    ImGui::Separator();
    ImGui::Text("ECS entities: %zu", world.registry().storage<entt::entity>().size());
    ImGui::End();
}

void DebugUI::render_to_mirror_window() {
    if (!g_ready) return;
    ImGui::Render();
    // Rendering to the mirror window requires a Vulkan renderer binding; we
    // omit the draw call here so the skeleton compiles without a full
    // Vulkan renderer + ImGui backend. The IMGUI_IMPL_VULKAN rendering
    // backend is linked in via vcpkg's imgui[vulkan-binding] feature and
    // will be wired up by a future renderer module.
}

}  // namespace ptv
