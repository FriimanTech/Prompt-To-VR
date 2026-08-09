cmake_minimum_required(VERSION 3.26)

# ---------------------------------------------------------------------------
# ptv_runtime: reusable, hand-authored VR engine core
# ---------------------------------------------------------------------------

file(GLOB_RECURSE PTV_RUNTIME_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

# Platform selection. This CMakeLists is the DESKTOP / PC VR build (Windows +
# GLFW + Win32 OpenXR). The Quest/Android build uses its own native CMakeLists
# under engine/android/src/main/cpp/ and defines XR_USE_PLATFORM_ANDROID. Drop
# the Android-only platform sources so they never reach the desktop compiler
# (they include <android/...> / <jni.h>).
list(FILTER PTV_RUNTIME_SOURCES EXCLUDE REGEX "/src/platform/android/")

add_library(ptv_runtime STATIC ${PTV_RUNTIME_SOURCES})

target_include_directories(ptv_runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_include_directories(ptv_runtime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(ptv_runtime
  PUBLIC
    Vulkan::Vulkan
    OpenXR::openxr_loader
    glfw
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    glm::glm
    EnTT::EnTT
    Jolt::Jolt
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    imgui::imgui
    ${PTV_TINYGLTF_TARGET}
)

target_compile_features(ptv_runtime PUBLIC cxx_std_20)
target_compile_definitions(ptv_runtime PUBLIC
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_RADIANS
  GLM_ENABLE_EXPERIMENTAL
  VK_NO_PROTOTYPES
  XR_USE_GRAPHICS_API_VULKAN
  NOMINMAX
)

# OpenXR platform macro is platform-specific. Desktop/PC VR is Windows-only
# here; the Quest/Android native build defines XR_USE_PLATFORM_ANDROID in its
# own CMakeLists instead.
if(WIN32)
  target_compile_definitions(ptv_runtime PUBLIC XR_USE_PLATFORM_WIN32)
endif()

# Shader compilation (glslc from Vulkan SDK).
find_program(PTV_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
if(PTV_GLSLC)
  file(GLOB PTV_SHADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp"
  )
  set(PTV_SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${PTV_SPV_DIR}")
  set(PTV_SPV_FILES)
  foreach(shader ${PTV_SHADERS})
    get_filename_component(shname "${shader}" NAME)
    set(spv_out "${PTV_SPV_DIR}/${shname}.spv")
    add_custom_command(
      OUTPUT "${spv_out}"
      COMMAND "${PTV_GLSLC}" -O "${shader}" -o "${spv_out}"
      DEPENDS "${shader}"
      COMMENT "Compiling shader: ${shname}"
      VERBATIM
    )
    list(APPEND PTV_SPV_FILES "${spv_out}")
  endforeach()
  add_custom_target(ptv_shaders ALL DEPENDS ${PTV_SPV_FILES})
  add_dependencies(ptv_runtime ptv_shaders)
  target_compile_definitions(ptv_runtime PUBLIC "PTV_SHADER_DIR=\"${PTV_SPV_DIR}\"")
else()
  message(WARNING "glslc not found. SPIR-V shaders will not be compiled automatically.")
  target_compile_definitions(ptv_runtime PUBLIC "PTV_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\"")
endif()

# ---------------------------------------------------------------------------
# Engine-owned runtime assets: the rigged player hands.
#
# Generated rather than committed for the same reason the test fixtures are:
# a deterministic script beats a binary in git, and Tripo cannot produce this
# asset at all (its rig types are all whole creatures, so a disembodied hand
# is not riggable). Resolved through PTV_ENGINE_ASSET_DIR, mirroring how
# PTV_SHADER_DIR works -- an absolute build path on desktop, a bare relative
# path inside the APK on Quest.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)

set(PTV_ENGINE_ASSET_DIR "${CMAKE_BINARY_DIR}/assets")
if(Python3_Interpreter_FOUND)
  set(PTV_HAND_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../tools/make_hand_fixture.py")
  set(PTV_HAND_FILES)
  foreach(hand left right)
    set(hand_out "${PTV_ENGINE_ASSET_DIR}/hand_${hand}.glb")
    add_custom_command(
      OUTPUT "${hand_out}"
      COMMAND "${Python3_EXECUTABLE}" "${PTV_HAND_SCRIPT}"
              --output "${hand_out}" --hand ${hand}
      DEPENDS "${PTV_HAND_SCRIPT}"
      COMMENT "Generating rigged ${hand} hand"
      VERBATIM
    )
    list(APPEND PTV_HAND_FILES "${hand_out}")
  endforeach()
  add_custom_target(ptv_engine_assets ALL DEPENDS ${PTV_HAND_FILES})
  add_dependencies(ptv_runtime ptv_engine_assets)
else()
  message(WARNING
    "Python3 not found; rigged hand meshes will not be generated. The runtime "
    "falls back to the procedural block hands.")
endif()
target_compile_definitions(ptv_runtime PUBLIC
  "PTV_ENGINE_ASSET_DIR=\"${PTV_ENGINE_ASSET_DIR}\"")
