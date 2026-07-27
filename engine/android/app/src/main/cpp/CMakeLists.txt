cmake_minimum_required(VERSION 3.22.1)

# ---------------------------------------------------------------------------
# Prompt-To-VR Quest app native library (libptv_quest.so)
#
# This is the Quest/Android counterpart to the desktop engine/runtime +
# samples/starter-room CMake. It is invoked by Gradle's externalNativeBuild
# with the NDK toolchain, so we do NOT use vcpkg here; third-party deps are
# fetched/built from source for arm64-android (see PTV dependency section).
#
# Cache vars passed from app/build.gradle:
#   PTV_ENGINE_DIR    absolute path to <repo>/engine
#   PTV_PROJECT_ID    the project id (selects engine/generated/**/<id>)
#   PTV_APP_LIB_NAME  output lib name (must match android.app.lib_name)
# ---------------------------------------------------------------------------
project(ptv_quest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED PTV_ENGINE_DIR)
  message(FATAL_ERROR "PTV_ENGINE_DIR not set (pass -DPTV_ENGINE_DIR=<repo>/engine)")
endif()
if(NOT DEFINED PTV_APP_LIB_NAME)
  set(PTV_APP_LIB_NAME ptv_quest)
endif()

set(PTV_RUNTIME_DIR "${PTV_ENGINE_DIR}/runtime")
set(PTV_GENERATED_DIR "${PTV_ENGINE_DIR}/generated")

# ---------------------------------------------------------------------------
# Third-party dependencies (arm64-android, from source).
#
# Header-only / small libs come via FetchContent. Jolt and spdlog are the two
# that actually compile TUs; they support Android but are the most likely to
# need version pinning. This is the first thing to iterate on if the initial
# device build fails.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_Declare(entt GIT_REPOSITORY https://github.com/skypjack/entt.git GIT_TAG v3.13.2)
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.1.0)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG 1.3.270)
FetchContent_Declare(tinygltf GIT_REPOSITORY https://github.com/syoyo/tinygltf.git GIT_TAG v2.9.2)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.14.1)
FetchContent_Declare(JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.2.0
  SOURCE_SUBDIR Build)

set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm entt nlohmann_json vma volk tinygltf spdlog JoltPhysics)

# The Khronos OpenXR Android loader (AAR/prefab) provides OpenXR::openxr_loader.
find_package(OpenXR REQUIRED CONFIG)

# Vulkan + Android system libs from the NDK.
find_library(ANDROID_LIB android)
find_library(LOG_LIB log)

# android_native_app_glue ships with the NDK.
add_library(native_app_glue STATIC
  "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC
  "${ANDROID_NDK}/sources/android/native_app_glue")

# ---------------------------------------------------------------------------
# Engine runtime sources (shared with desktop) minus the desktop platform dir,
# plus the Android platform dir, plus this project's generated mechanics.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PTV_RUNTIME_SOURCES CONFIGURE_DEPENDS
  "${PTV_RUNTIME_DIR}/src/*.cpp")
list(FILTER PTV_RUNTIME_SOURCES EXCLUDE REGEX "/src/platform/desktop/")

file(GLOB_RECURSE PTV_GENERATED_SOURCES CONFIGURE_DEPENDS
  "${PTV_GENERATED_DIR}/mechanics/${PTV_PROJECT_ID}/*.cpp"
  "${PTV_GENERATED_DIR}/ai/${PTV_PROJECT_ID}/*.cpp"
  "${PTV_GENERATED_DIR}/dialogue/${PTV_PROJECT_ID}/*.cpp")

add_library(${PTV_APP_LIB_NAME} SHARED
  ${PTV_RUNTIME_SOURCES}
  ${PTV_GENERATED_SOURCES})

target_include_directories(${PTV_APP_LIB_NAME} PRIVATE
  "${PTV_RUNTIME_DIR}/include"
  "${PTV_RUNTIME_DIR}/src")

target_compile_definitions(${PTV_APP_LIB_NAME} PRIVATE
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_RADIANS
  GLM_ENABLE_EXPERIMENTAL
  VK_NO_PROTOTYPES
  VK_USE_PLATFORM_ANDROID_KHR
  XR_USE_GRAPHICS_API_VULKAN
  XR_USE_PLATFORM_ANDROID
  # Shaders are read from the APK assets root via AAssetManager.
  PTV_SHADER_DIR="shaders")

target_link_libraries(${PTV_APP_LIB_NAME} PRIVATE
  native_app_glue
  OpenXR::openxr_loader
  glm::glm
  EnTT::EnTT
  nlohmann_json::nlohmann_json
  GPUOpen::VulkanMemoryAllocator
  volk::volk
  Jolt
  spdlog::spdlog
  ${ANDROID_LIB}
  ${LOG_LIB})

target_include_directories(${PTV_APP_LIB_NAME} PRIVATE
  "${tinygltf_SOURCE_DIR}")

# NativeActivity's ANativeActivity_onCreate is provided by native_app_glue;
# keep it even though nothing in our code references it directly.
target_link_options(${PTV_APP_LIB_NAME} PRIVATE "-u ANativeActivity_onCreate")

# Force-link every generated mechanic object so its PTV_REGISTER_* static
# initializer runs (same rationale as /WHOLEARCHIVE on desktop). Without this
# the linker drops self-registering mechanics that are only looked up by
# string at runtime.
if(PTV_GENERATED_SOURCES)
  target_link_options(${PTV_APP_LIB_NAME} PRIVATE
    "-Wl,--whole-archive" "-Wl,--no-whole-archive")
endif()
