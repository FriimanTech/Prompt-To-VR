cmake_minimum_required(VERSION 3.26)

# Console subsystem (regular int main) so we can see spdlog output alongside
# the GLFW mirror window while testing XR.
add_executable(ptv_starter_room main.cpp)
target_link_libraries(ptv_starter_room PRIVATE ptv_runtime)
target_compile_features(ptv_starter_room PRIVATE cxx_std_20)

# Force-link the whole ptv_runtime archive into the executable.
#
# Generated gameplay mechanics (engine/generated/**) are compiled INTO
# ptv_runtime and register themselves via PTV_REGISTER_MECHANIC static
# initializers. With a normal static-library link, MSVC (and ld) only pull
# object files that define a referenced symbol -- and a self-registering
# mechanic is referenced by NOTHING at compile time (the scene loader looks
# it up by string at runtime). So the linker would DROP those .obj files and
# the registrations would never run, leaving every generated mechanic
# "not registered" at play time. /WHOLEARCHIVE (MSVC) / --whole-archive
# (GNU/Clang) forces every object -- including the generated ones -- into the
# exe so their registrations execute. The engine registry map is a
# function-local static, so registration during static-init is safe.
# See https://learn.microsoft.com/cpp/build/reference/wholearchive.
if(MSVC)
  target_link_options(ptv_starter_room PRIVATE
    "/WHOLEARCHIVE:$<TARGET_FILE:ptv_runtime>")
else()
  target_link_options(ptv_starter_room PRIVATE
    "LINKER:--whole-archive,$<TARGET_FILE:ptv_runtime>,--no-whole-archive")
endif()

# Per-project executable name. The orchestrator passes -DPTV_APP_NAME="<title>"
# so each generated game builds as ``<Project Title>.exe`` instead of the
# generic ``ptv_starter_room.exe``. We only override the OUTPUT_NAME (the file
# on disk); the CMake target stays ``ptv_starter_room`` so every generator
# expression / dependency below keeps working. The Quest Link "recents" label
# itself comes from the OpenXR application name, which the runtime derives from
# the scene's title at launch (see Application::initialize).
if(DEFINED PTV_APP_NAME AND NOT PTV_APP_NAME STREQUAL "")
  set_target_properties(ptv_starter_room PROPERTIES OUTPUT_NAME "${PTV_APP_NAME}")
endif()

# Copy the built-in scene JSON next to the executable so it can run with no
# arguments.
add_custom_command(TARGET ptv_starter_room POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/scene.json"
          "$<TARGET_FILE_DIR:ptv_starter_room>/scene.json"
)
set_target_properties(ptv_starter_room PROPERTIES
  FOLDER "ptv/samples"
  VS_DPI_AWARE "PerMonitor"
)
