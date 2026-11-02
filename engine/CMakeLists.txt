cmake_minimum_required(VERSION 3.26)

project(ptv_engine
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  # Quiet common noise from third-party headers.
  add_compile_options(/wd4100 /wd4189 /wd4324)
endif()

# ---------------------------------------------------------------------------
# Third-party packages (vcpkg manifest mode)
# ---------------------------------------------------------------------------

find_package(Vulkan REQUIRED)
find_package(OpenXR CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(TinyGLTF CONFIG QUIET)
# stb_truetype: bakes the SDF glyph atlas for world-space text (menus, HUD).
# Header-only; vcpkg's port exposes the include dir as Stb_INCLUDE_DIR.
find_package(Stb REQUIRED)
# Offline mesh simplification for the asset_optimization pipeline stage
# (engine/tools/glb_optimize.cpp). Host-only: not linked into the runtime.
find_package(meshoptimizer CONFIG REQUIRED)
# Offline ASTC texture compression (libktx bundles ARM's astcenc) for the
# asset_optimization stage (engine/tools/texture_compress.cpp). Host-only:
# the runtime reads the resulting KTX2 files with its own small parser.
find_package(Ktx CONFIG QUIET)
if(NOT Ktx_FOUND)
  message(WARNING "KTX-Software (vcpkg port 'ktx') not found: ptv_texture_compress "
                  "will not be built, and Quest builds fall back to decoding "
                  "PNG/JPEG textures on the headset.")
endif()

# Some vcpkg portfile versions expose tinygltf as an INTERFACE target or via
# a different package name; handle both cases gracefully.
if(NOT TARGET TinyGLTF::TinyGLTF AND NOT TARGET tinygltf)
  find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h)
  if(TINYGLTF_INCLUDE_DIR)
    add_library(tinygltf INTERFACE)
    target_include_directories(tinygltf INTERFACE ${TINYGLTF_INCLUDE_DIR})
  endif()
endif()

if(TARGET TinyGLTF::TinyGLTF)
  set(PTV_TINYGLTF_TARGET TinyGLTF::TinyGLTF)
elseif(TARGET tinygltf)
  set(PTV_TINYGLTF_TARGET tinygltf)
else()
  message(FATAL_ERROR "tinygltf was not found via vcpkg. Install with `vcpkg install tinygltf`.")
endif()

# ---------------------------------------------------------------------------
# Runtime library
# ---------------------------------------------------------------------------

add_subdirectory(runtime)
add_subdirectory(tools)

# Sample hand-authored demo (no LLM needed to run this).
if(EXISTS "${CMAKE_SOURCE_DIR}/../samples/starter-room/CMakeLists.txt")
  add_subdirectory("${CMAKE_SOURCE_DIR}/../samples/starter-room" samples/starter-room)
endif()

# Generated gameplay modules (optional; per-project overlay).
if(EXISTS "${CMAKE_SOURCE_DIR}/generated/CMakeLists.txt")
  add_subdirectory(generated)
endif()

# Offline world-generation host (ptv_worldgen). Off by default: the
# orchestrator's world_generation stage usually configures engine/worldgen as a
# standalone project for fast compile-and-repair rebuilds. This option only
# makes it visible in the full engine build / IDE.
option(PTV_BUILD_WORLDGEN "Build the offline world-gen host (ptv_worldgen)" OFF)
if(PTV_BUILD_WORLDGEN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/worldgen/CMakeLists.txt")
  add_subdirectory(worldgen)
endif()

# Tests.
enable_testing()
if(EXISTS "${CMAKE_SOURCE_DIR}/runtime/tests/CMakeLists.txt")
  add_subdirectory(runtime/tests)
endif()
