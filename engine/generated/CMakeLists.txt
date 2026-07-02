cmake_minimum_required(VERSION 3.26)

# Glob every generated C++ source file under the three allowed subtrees.
# Generated files compile into ptv_runtime so they see its headers and so
# that the PTV_REGISTER_MECHANIC macro runs at static init time.
#
# Generated .h/.hpp headers are NOT globbed into SOURCES -- they live
# alongside the .cpp files and are resolved automatically by the compiler
# via #include "foo.h" (which searches the including file's directory first).
# No extra target_include_directories is needed.

file(GLOB_RECURSE PTV_GENERATED_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/mechanics/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/mechanics/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ai/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ai/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dialogue/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dialogue/*.cpp"
)

if(PTV_GENERATED_SOURCES)
  list(LENGTH PTV_GENERATED_SOURCES _ptv_gen_count)
  target_sources(ptv_runtime PRIVATE ${PTV_GENERATED_SOURCES})
  message(STATUS "Linking ${_ptv_gen_count} generated gameplay source files")
endif()
