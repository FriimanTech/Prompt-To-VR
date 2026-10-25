cmake_minimum_required(VERSION 3.26)

# Header-only assertion macro is enough for smoke tests.
add_executable(ptv_smoke smoke.cpp)
target_link_libraries(ptv_smoke PRIVATE ptv_runtime)
set_target_properties(ptv_smoke PROPERTIES FOLDER "ptv/tests")

add_test(NAME ptv_smoke_mechanics
         COMMAND ptv_smoke mechanics)
add_test(NAME ptv_smoke_scene_loader
         COMMAND ptv_smoke scene)
add_test(NAME ptv_smoke_stationary_scene
         COMMAND ptv_smoke stationary_scene)
add_test(NAME ptv_smoke_locomotion_variants
         COMMAND ptv_smoke locomotion_variants)
# A real Uthana rig (Armature scale 0.01, 90 deg X) with one merged walk clip.
add_test(NAME ptv_smoke_uthana_root_bake
         COMMAND ptv_smoke uthana_root_bake
                 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/uthana_walk_merged.glb")
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
  add_test(NAME ptv_smoke_clip_blend
           COMMAND ptv_smoke clip_blend "${PTV_RIGGED_FIXTURE}")
  add_test(NAME ptv_smoke_animation_lod
           COMMAND ptv_smoke animation_lod "${PTV_RIGGED_FIXTURE}")
  add_test(NAME ptv_smoke_self_view_hide
           COMMAND ptv_smoke self_view_hide "${PTV_RIGGED_FIXTURE}")
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

  # The player's default rigged hands. Built by the same engine target the
  # runtime depends on, so the tests exercise exactly the asset that ships.
  set(PTV_HAND_GLB "${CMAKE_BINARY_DIR}/assets/default_hand_right.glb")
  if(TARGET ptv_engine_assets)
    add_dependencies(ptv_smoke ptv_engine_assets)
    add_test(NAME ptv_smoke_hand_asset
             COMMAND ptv_smoke hand_asset "${PTV_HAND_GLB}")
    add_test(NAME ptv_smoke_hand_pose
             COMMAND ptv_smoke hand_pose "${PTV_HAND_GLB}")
  endif()
else()
  message(WARNING
    "Python3 not found; skipping the rigged GLB fixture and the skinning "
    "smoke tests.")
endif()

# These two need no generated asset at all -- one is pure transform maths, the
# other parses an inline scene -- so they run even without Python.
add_test(NAME ptv_smoke_body_slots COMMAND ptv_smoke body_slots)
add_test(NAME ptv_smoke_two_bone_ik COMMAND ptv_smoke two_bone_ik)
add_test(NAME ptv_smoke_mirror_reflection COMMAND ptv_smoke mirror_reflection)
add_test(NAME ptv_smoke_equip_semantics COMMAND ptv_smoke equip_semantics)
add_test(NAME ptv_smoke_equip_slots COMMAND ptv_smoke equip_slots)
add_test(NAME ptv_smoke_interactive_persistence
         COMMAND ptv_smoke interactive_persistence)
add_test(NAME ptv_smoke_affordance_state COMMAND ptv_smoke affordance_state)
add_test(NAME ptv_smoke_visual_tags COMMAND ptv_smoke visual_tags)
add_test(NAME ptv_smoke_scene_transitions COMMAND ptv_smoke scene_transitions)
add_test(NAME ptv_smoke_ui_panel COMMAND ptv_smoke ui_panel)
add_test(NAME ptv_smoke_reference_frame COMMAND ptv_smoke reference_frame)
add_test(NAME ptv_smoke_nested_scene COMMAND ptv_smoke nested_scene)
add_test(NAME ptv_smoke_grab_socket COMMAND ptv_smoke grab_socket)
add_test(NAME ptv_smoke_clip_contract COMMAND ptv_smoke clip_contract)
add_test(NAME ptv_smoke_physics_capsule COMMAND ptv_smoke physics_capsule)
add_test(NAME ptv_smoke_physics_player_slide
         COMMAND ptv_smoke physics_player_slide)
add_test(NAME ptv_smoke_physics_overlap COMMAND ptv_smoke physics_overlap)
add_test(NAME ptv_smoke_player_character COMMAND ptv_smoke player_character)
add_test(NAME ptv_smoke_player_self_push COMMAND ptv_smoke player_self_push)
add_test(NAME ptv_smoke_sprint_gate COMMAND ptv_smoke sprint_gate)
add_test(NAME ptv_smoke_avatar_anchor COMMAND ptv_smoke avatar_anchor)
add_test(NAME ptv_smoke_height_calibration COMMAND ptv_smoke height_calibration)
add_test(NAME ptv_smoke_arm_assist COMMAND ptv_smoke arm_assist)
add_test(NAME ptv_smoke_teleport_pitch COMMAND ptv_smoke teleport_pitch)
add_test(NAME ptv_smoke_runtime_spawn COMMAND ptv_smoke runtime_spawn)
add_test(NAME ptv_smoke_clone_tree COMMAND ptv_smoke clone_tree)
add_test(NAME ptv_smoke_encounter_director COMMAND ptv_smoke encounter_director)
add_test(NAME ptv_smoke_npc_behaviors COMMAND ptv_smoke npc_behaviors)
add_test(NAME ptv_smoke_character_locomotion
         COMMAND ptv_smoke character_locomotion)
add_test(NAME ptv_smoke_attachment COMMAND ptv_smoke attachment)
add_test(NAME ptv_smoke_combat_factions COMMAND ptv_smoke combat_factions)
add_test(NAME ptv_smoke_nav_graph COMMAND ptv_smoke nav_graph)
add_test(NAME ptv_smoke_npc_ground_snap COMMAND ptv_smoke npc_ground_snap)
add_test(NAME ptv_smoke_mechanic_host_npc COMMAND ptv_smoke mechanic_host_npc)
add_test(NAME ptv_smoke_player_rig_adoption COMMAND ptv_smoke player_rig_adoption)
add_test(NAME ptv_smoke_damage_log COMMAND ptv_smoke damage_log)
