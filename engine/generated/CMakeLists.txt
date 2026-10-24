cmake_minimum_required(VERSION 3.26)

# Generated gameplay C++ for ONE game compiles into ptv_runtime, so it sees the
# runtime's headers and its PTV_REGISTER_MECHANIC macros run at static init.
#
# Only the game being built is compiled. Every game's code lives side by side
# under engine/generated/<kind>/<project_id>/, and two games routinely pick
# the same task names (two shooters both have a "shooting_hub"): compiled
# together they define the same symbols and register the same mechanic ids,
# and the link fails with LNK2005 for code that is correct on its own. The
# Quest build has always compiled just its own project (engine/android).
#
#   PTV_GENERATED_PROJECT_ID   the game to compile (the orchestrator passes it).
#                              Empty: no generated code (engine-only builds and
#                              tests). "*": every project (old behaviour).
#   PTV_GENERATED_EXTRA_SOURCES  extra .cpp files to compile with it -- mechanics
#                              promoted from another game that this game's plan
#                              reuses.
#
# Generated .h/.hpp headers are NOT globbed into SOURCES -- they live
# alongside the .cpp files and are resolved automatically by the compiler
# via #include "foo.h" (which searches the including file's directory first).

set(PTV_GENERATED_PROJECT_ID "" CACHE STRING
    "Project whose generated C++ to compile (\"*\" = all, empty = none)")
set(PTV_GENERATED_EXTRA_SOURCES "" CACHE STRING
    "Additional generated .cpp files (promoted mechanics from other projects)")

set(PTV_GENERATED_SOURCES "")
if(PTV_GENERATED_PROJECT_ID STREQUAL "*")
  file(GLOB_RECURSE PTV_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/mechanics/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dialogue/*.cpp"
  )
elseif(PTV_GENERATED_PROJECT_ID)
  file(GLOB_RECURSE PTV_GENERATED_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/mechanics/${PTV_GENERATED_PROJECT_ID}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ai/${PTV_GENERATED_PROJECT_ID}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dialogue/${PTV_GENERATED_PROJECT_ID}/*.cpp"
  )
endif()

foreach(_ptv_extra IN LISTS PTV_GENERATED_EXTRA_SOURCES)
  if(EXISTS "${_ptv_extra}")
    list(APPEND PTV_GENERATED_SOURCES "${_ptv_extra}")
  else()
    message(WARNING "Promoted source not found, skipped: ${_ptv_extra}")
  endif()
endforeach()
list(REMOVE_DUPLICATES PTV_GENERATED_SOURCES)

if(PTV_GENERATED_SOURCES)
  list(LENGTH PTV_GENERATED_SOURCES _ptv_gen_count)
  target_sources(ptv_runtime PRIVATE ${PTV_GENERATED_SOURCES})
  message(STATUS "Linking ${_ptv_gen_count} generated gameplay source files "
                 "(project: ${PTV_GENERATED_PROJECT_ID})")
else()
  message(STATUS "No generated gameplay sources (PTV_GENERATED_PROJECT_ID='${PTV_GENERATED_PROJECT_ID}')")
endif()
