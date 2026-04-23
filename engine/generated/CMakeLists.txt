cmake_minimum_required(VERSION 3.26)

# Glob every generated C++ file under the three allowed subtrees. Generated
# files compile into ptv_runtime so they see its headers and so that the
# PTV_REGISTER_MECHANIC macro runs at static init time.

file(GLOB_RECURSE PTV_GENERATED_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/mechanics/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/mechanics/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ai/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/ai/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dialogue/**/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/dialogue/*.cpp"
)

if(PTV_GENERATED_SOURCES)
  target_sources(ptv_runtime PRIVATE ${PTV_GENERATED_SOURCES})
  message(STATUS "Linking ${CMAKE_MATCH_COUNT}+ generated gameplay source files")
endif()
