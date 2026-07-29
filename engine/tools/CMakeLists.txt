cmake_minimum_required(VERSION 3.26)

# glb_validate: tiny CLI that opens a GLB and prints a short report.
add_executable(ptv_glb_validate glb_validate.cpp)
target_link_libraries(ptv_glb_validate PRIVATE ptv_runtime)
set_target_properties(ptv_glb_validate PROPERTIES FOLDER "ptv/tools")

# glb_optimize: offline decimation + LOD generation for the pipeline's
# asset_optimization stage. Reuses the runtime's GLB merge/colour-bake so the
# optimised mesh is byte-for-byte what the engine would have loaded.
add_executable(ptv_glb_optimize glb_optimize.cpp)
target_link_libraries(ptv_glb_optimize PRIVATE
  ptv_runtime
  meshoptimizer::meshoptimizer)
set_target_properties(ptv_glb_optimize PROPERTIES FOLDER "ptv/tools")
