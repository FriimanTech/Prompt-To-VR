cmake_minimum_required(VERSION 3.26)

# ptv_worldgen: tiny OFFLINE host that runs generated IWorldBuilder code and
# emits world.scene.json. It links NONE of the engine core (no Vulkan, OpenXR,
# Jolt, renderer, ECS, or platform layer) -- only nlohmann-json (+ glm headers
# for author convenience). It can be configured standalone (fast rebuilds for
# the compile-and-repair loop) or added via add_subdirectory from engine/.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ptv_worldgen LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Engine root (engine/), one level up from engine/worldgen/.
get_filename_component(PTV_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json CONFIG REQUIRED)
endif()
if(NOT TARGET glm::glm)
  find_package(glm CONFIG QUIET)
endif()

set(PTV_WORLDGEN_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/WorldGen.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/builtin/fallback_world.cpp"
)

# Per-project generated world builder(s), selected by project id. The stage
# passes -DPTV_WORLD_PROJECT_ID=<id> so only that project's world code compiles.
if(DEFINED PTV_WORLD_PROJECT_ID AND NOT "${PTV_WORLD_PROJECT_ID}" STREQUAL "")
  # PTV_WORLD_ENV_ID narrows the glob to ONE environment's subdirectory.
  #
  # This is what makes several environments per project safe to generate in
  # parallel. Without it every environment's C++ compiles into one binary, so
  # they share a translation-unit set: two agents fight over identical
  # filenames, duplicate helper types collide at namespace scope, and a syntax
  # error in the head-interior fails the compile for the main world and burns
  # its repair budget on someone else's mistake.
  #
  # Left empty, the glob takes the whole project directory, which is the old
  # single-environment behaviour and keeps existing projects building.
  if(DEFINED PTV_WORLD_ENV_ID AND NOT "${PTV_WORLD_ENV_ID}" STREQUAL "")
    file(GLOB_RECURSE PTV_WORLD_GENERATED CONFIGURE_DEPENDS
      "${PTV_ENGINE_DIR}/generated/world/${PTV_WORLD_PROJECT_ID}/${PTV_WORLD_ENV_ID}/*.cpp")
  else()
    file(GLOB_RECURSE PTV_WORLD_GENERATED CONFIGURE_DEPENDS
      "${PTV_ENGINE_DIR}/generated/world/${PTV_WORLD_PROJECT_ID}/*.cpp")
  endif()
  list(APPEND PTV_WORLDGEN_SOURCES ${PTV_WORLD_GENERATED})
endif()

# Optional hand-authored samples for no-LLM validation.
option(PTV_WORLDGEN_SAMPLES "Compile hand-authored world samples" OFF)
if(PTV_WORLDGEN_SAMPLES)
  file(GLOB PTV_WORLDGEN_SAMPLE_SRC CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/*.cpp")
  list(APPEND PTV_WORLDGEN_SOURCES ${PTV_WORLDGEN_SAMPLE_SRC})
endif()

add_executable(ptv_worldgen ${PTV_WORLDGEN_SOURCES})
target_include_directories(ptv_worldgen PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${PTV_ENGINE_DIR}/runtime/include"
)
target_link_libraries(ptv_worldgen PRIVATE nlohmann_json::nlohmann_json)
if(TARGET glm::glm)
  target_link_libraries(ptv_worldgen PRIVATE glm::glm)
endif()
target_compile_features(ptv_worldgen PRIVATE cxx_std_20)
if(MSVC)
  target_compile_definitions(ptv_worldgen PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
set_target_properties(ptv_worldgen PROPERTIES FOLDER "ptv/worldgen")
