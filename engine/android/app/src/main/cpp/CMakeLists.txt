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
#   PTV_GENERATED_EXTRA_SOURCES  promoted .cpp files from other projects
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
# spdlog 1.15.3 bundles fmt 11.2.0. Older spdlog (1.14.x -> fmt 10.x) fails to
# compile with the NDK's modern clang: bundled fmt's FMT_STRING triggers "call
# to consteval function ... is not a constant expression" (fixed in fmt 11.2.0).
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.15.3)
# Must track the joltphysics version vcpkg resolves for the desktop build
# (currently 5.5.0, see JPH_VERSION_MAJOR/MINOR in its Jolt/Core/Core.h).
# When the two drift, desktop compiles engine code that the NDK rejects, and
# the only signal is a Quest-only build failure: v5.2.0 here predated
# Vec3::sOne, which desktop had, so PhysicsWorld.cpp built everywhere except
# the APK.
FetchContent_Declare(JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.5.0
  SOURCE_SUBDIR Build)

# stb (stb_truetype for the text atlas). Header-only, no CMake project, so it
# is only populated. Pinned to the commit vcpkg's "stb" port (2024-07-29)
# uses on desktop so both builds bake identical glyphs.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
  FetchContent_Populate(stb)
endif()

set(TINYGLTF_HEADER_ONLY ON CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm entt nlohmann_json vma volk tinygltf spdlog JoltPhysics)

# The Khronos OpenXR Android loader (AAR/prefab) provides OpenXR::openxr_loader.
find_package(OpenXR REQUIRED CONFIG)

# Vulkan + Android system libs from the NDK. volk loads Vulkan entry points at
# runtime via dlopen, but we still link the NDK's libvulkan.so so the loader
# symbols resolve and OpenXR's Vulkan graphics binding is satisfied.
find_library(VULKAN_LIB vulkan)
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
# Desktop-only translation units that must NOT be compiled for Quest:
#   * platform/desktop/ -> GLFW mirror window (needs GLFW).
#   * debug/            -> Dear ImGui panels (needs imgui + the GLFW backend),
#                          neither of which is fetched/linked for Android.
list(FILTER PTV_RUNTIME_SOURCES EXCLUDE REGEX "/src/platform/desktop/")
list(FILTER PTV_RUNTIME_SOURCES EXCLUDE REGEX "/src/debug/")

file(GLOB_RECURSE PTV_GENERATED_SOURCES CONFIGURE_DEPENDS
  "${PTV_GENERATED_DIR}/mechanics/${PTV_PROJECT_ID}/*.cpp"
  "${PTV_GENERATED_DIR}/ai/${PTV_PROJECT_ID}/*.cpp"
  "${PTV_GENERATED_DIR}/dialogue/${PTV_PROJECT_ID}/*.cpp")
# Mechanics promoted from another game that this game's plan reuses.
foreach(_ptv_extra IN LISTS PTV_GENERATED_EXTRA_SOURCES)
  if(EXISTS "${_ptv_extra}")
    list(APPEND PTV_GENERATED_SOURCES "${_ptv_extra}")
  endif()
endforeach()

# Shared compile definitions applied to both the engine runtime and the
# generated gameplay code (they must agree on the Vulkan/OpenXR platform and
# GLM configuration or ABI/layout mismatches appear at runtime).
set(PTV_COMPILE_DEFS
  GLM_FORCE_DEPTH_ZERO_TO_ONE
  GLM_FORCE_RADIANS
  GLM_ENABLE_EXPERIMENTAL
  VK_NO_PROTOTYPES
  VK_USE_PLATFORM_ANDROID_KHR
  XR_USE_GRAPHICS_API_VULKAN
  XR_USE_PLATFORM_ANDROID
  # Shaders and the generated hand meshes are read from the APK assets root
  # via AAssetManager, so both are bare relative paths here rather than the
  # absolute build-tree paths the desktop build uses.
  PTV_SHADER_DIR="shaders"
  PTV_ENGINE_ASSET_DIR="engine")

set(PTV_INCLUDE_DIRS
  "${PTV_RUNTIME_DIR}/include"
  "${PTV_RUNTIME_DIR}/src"
  "${tinygltf_SOURCE_DIR}"
  "${stb_SOURCE_DIR}")

# Warnings we refuse to hear from vendored headers.
#
# The NDK's clang wants EVERY pointer in a header annotated once any of them
# is, so VMA's vk_mem_alloc.h alone emits well over two thousand
# -Wnullability-completeness lines per translation unit that includes it.
# There is nothing to fix in a vendored header, and the noise is actively
# harmful: a one-line compile error sat invisible in a 2,377-line build log,
# and diagnosing the build that produced it needed the APK timestamp rather
# than the log. Silencing the pair here keeps real diagnostics findable.
# EnTT's `operator"" _hs` trips -Wdeprecated-literal-operator in every file
# that includes the ECS, for the same nothing-to-fix reason.
set(PTV_COMPILE_OPTS
  -Wno-nullability-completeness
  -Wno-nullability-extension
  -Wno-deprecated-literal-operator)

# Third-party usage requirements shared by the runtime and generated code.
# NOTE: JoltPhysics via FetchContent exposes the plain target name "Jolt" (the
# namespaced "Jolt::Jolt" alias only exists in the vcpkg package used on
# desktop), so we must link "Jolt" here or CMake generate fails.
set(PTV_THIRDPARTY_LIBS
  glm::glm
  EnTT::EnTT
  nlohmann_json::nlohmann_json
  GPUOpen::VulkanMemoryAllocator
  volk::volk
  Jolt
  spdlog::spdlog)

# ---------------------------------------------------------------------------
# Generated gameplay mechanics compile into their OWN static library so we can
# force the linker to retain every self-registering translation unit. Each
# generated .cpp calls PTV_REGISTER_MECHANIC(...) from a static initializer and
# is referenced only by string at runtime, so without --whole-archive the
# linker discards the object and the mechanic is "not registered" at play time
# (same rationale as /WHOLEARCHIVE on desktop -- see samples/starter-room).
# ---------------------------------------------------------------------------
if(PTV_GENERATED_SOURCES)
  add_library(ptv_generated STATIC ${PTV_GENERATED_SOURCES})
  target_include_directories(ptv_generated PRIVATE ${PTV_INCLUDE_DIRS})
  target_compile_definitions(ptv_generated PRIVATE ${PTV_COMPILE_DEFS})
  target_compile_options(ptv_generated PRIVATE ${PTV_COMPILE_OPTS})
  target_link_libraries(ptv_generated PRIVATE ${PTV_THIRDPARTY_LIBS})
endif()

add_library(${PTV_APP_LIB_NAME} SHARED ${PTV_RUNTIME_SOURCES})

target_include_directories(${PTV_APP_LIB_NAME} PRIVATE ${PTV_INCLUDE_DIRS})
target_compile_definitions(${PTV_APP_LIB_NAME} PRIVATE ${PTV_COMPILE_DEFS})
target_compile_options(${PTV_APP_LIB_NAME} PRIVATE ${PTV_COMPILE_OPTS})

# native_app_glue is whole-archived so ANativeActivity_onCreate (the
# NativeActivity entry point, referenced only by the Android framework via
# dlsym, never by our code) is included AND exported in libptv_quest.so.
# Without this the loader fails with
#   UnsatisfiedLinkError: ... undefined symbol: ANativeActivity_onCreate
target_link_libraries(${PTV_APP_LIB_NAME} PRIVATE
  "-Wl,--whole-archive"
  native_app_glue
  "-Wl,--no-whole-archive"
  OpenXR::openxr_loader
  ${PTV_THIRDPARTY_LIBS}
  ${VULKAN_LIB}
  ${ANDROID_LIB}
  ${LOG_LIB})

# Force-link the generated mechanic archive whole so PTV_REGISTER_* static
# initializers survive. The raw -Wl flags surround the target on the link line;
# CMake expands ${ptv_generated} to its archive path in place.
if(PTV_GENERATED_SOURCES)
  target_link_libraries(${PTV_APP_LIB_NAME} PRIVATE
    "-Wl,--whole-archive"
    ptv_generated
    "-Wl,--no-whole-archive")
endif()

# Belt-and-suspenders alongside the whole-archive above: force the linker to
# treat ANativeActivity_onCreate as a needed symbol. The SHELL: prefix is
# REQUIRED so CMake passes "-u" and the symbol as two separate arguments; a
# single quoted "-u ANativeActivity_onCreate" string is delivered as one
# malformed argument and silently does nothing.
target_link_options(${PTV_APP_LIB_NAME} PRIVATE "SHELL:-u ANativeActivity_onCreate")
