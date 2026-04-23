cmake_minimum_required(VERSION 3.26)

# glb_validate: tiny CLI that opens a GLB and prints a short report.
add_executable(ptv_glb_validate glb_validate.cpp)
target_link_libraries(ptv_glb_validate PRIVATE ptv_runtime)
set_target_properties(ptv_glb_validate PROPERTIES FOLDER "ptv/tools")
