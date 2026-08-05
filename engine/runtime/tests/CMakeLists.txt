cmake_minimum_required(VERSION 3.26)

# Header-only assertion macro is enough for smoke tests.
add_executable(ptv_smoke smoke.cpp)
target_link_libraries(ptv_smoke PRIVATE ptv_runtime)
set_target_properties(ptv_smoke PROPERTIES FOLDER "ptv/tests")

add_test(NAME ptv_smoke_mechanics
         COMMAND ptv_smoke mechanics)
add_test(NAME ptv_smoke_scene_loader
         COMMAND ptv_smoke scene)
add_test(NAME ptv_smoke_openxr_detect
         COMMAND ptv_smoke openxr)
# openxr_detect passes even without a runtime; it just prints whether the
# loader could create an instance.

# ---------------------------------------------------------------------------
# Rigged test fixture
#
# No pipeline stage produces a rigged GLB yet, so the skinning tests generate
# their own input. The generator script is committed; its output is not, which
# keeps binary assets out of git and the fixture deterministic and offline.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
  set(PTV_RIGGED_FIXTURE "${CMAKE_BINARY_DIR}/fixtures/rigged_column.glb")
  set(PTV_STATIC_FIXTURE "${CMAKE_BINARY_DIR}/fixtures/pivot_column.glb")
  set(PTV_FIXTURE_SCRIPT
      "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/make_rigged_fixture.py")
  add_custom_command(
    OUTPUT "${PTV_RIGGED_FIXTURE}" "${PTV_STATIC_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${PTV_FIXTURE_SCRIPT}"
            --output "${PTV_RIGGED_FIXTURE}"
    COMMAND "${Python3_EXECUTABLE}" "${PTV_FIXTURE_SCRIPT}"
            --output "${PTV_STATIC_FIXTURE}" --no-skin
    DEPENDS "${PTV_FIXTURE_SCRIPT}"
    COMMENT "Generating rigged + pivoted GLB fixtures"
    VERBATIM)
  add_custom_target(ptv_rigged_fixture ALL
                    DEPENDS "${PTV_RIGGED_FIXTURE}" "${PTV_STATIC_FIXTURE}")
  set_target_properties(ptv_rigged_fixture PROPERTIES FOLDER "ptv/tests")
  add_dependencies(ptv_smoke ptv_rigged_fixture)

  add_test(NAME ptv_smoke_skinning
           COMMAND ptv_smoke skinning "${PTV_RIGGED_FIXTURE}")
  add_test(NAME ptv_smoke_gltf_nodes
           COMMAND ptv_smoke gltf_nodes "${PTV_STATIC_FIXTURE}")
  add_test(NAME ptv_smoke_skinning_math
           COMMAND ptv_smoke skinning_math "${PTV_RIGGED_FIXTURE}")
  add_test(NAME ptv_smoke_pose_system
           COMMAND ptv_smoke pose_system "${PTV_RIGGED_FIXTURE}")
  # Needs a real device; passes with an informational note when none exists.
  add_test(NAME ptv_smoke_skinning_gpu
           COMMAND ptv_smoke skinning_gpu "${PTV_RIGGED_FIXTURE}")

  # Decimate the fixture into a full LOD chain, then assert every level still
  # carries the same rig. --sloppy is passed for the coarse levels exactly as
  # stage_asset_optimize.py does, so the tool's refusal to apply it to rigged
  # input is exercised here too.
  set(PTV_LOD_DIR "${CMAKE_BINARY_DIR}/fixtures/lod")
  set(PTV_LOD_FILES)
  set(PTV_LOD_COMMANDS)
  set(_lod_targets 404 161 60 20)
  foreach(_lvl RANGE 3)
    list(GET _lod_targets ${_lvl} _tris)
    set(_out "${PTV_LOD_DIR}/rigged_lod${_lvl}.glb")
    list(APPEND PTV_LOD_FILES "${_out}")
    if(_lvl GREATER 0)
      set(_sloppy --sloppy)
    else()
      set(_sloppy)
    endif()
    list(APPEND PTV_LOD_COMMANDS
         COMMAND ptv_glb_optimize --input "${PTV_RIGGED_FIXTURE}"
                 --output "${_out}" --target-tris ${_tris} ${_sloppy})
  endforeach()

  add_custom_command(
    OUTPUT ${PTV_LOD_FILES}
    ${PTV_LOD_COMMANDS}
    DEPENDS ptv_glb_optimize "${PTV_RIGGED_FIXTURE}"
    COMMENT "Decimating the rigged fixture into a LOD chain"
    VERBATIM)
  add_custom_target(ptv_rigged_lods ALL DEPENDS ${PTV_LOD_FILES})
  set_target_properties(ptv_rigged_lods PROPERTIES FOLDER "ptv/tests")
  add_dependencies(ptv_smoke ptv_rigged_lods)

  add_test(NAME ptv_smoke_lod_skinning
           COMMAND ptv_smoke lod_skinning ${PTV_LOD_FILES})
else()
  message(WARNING
    "Python3 not found; skipping the rigged GLB fixture and the skinning "
    "smoke tests.")
endif()
