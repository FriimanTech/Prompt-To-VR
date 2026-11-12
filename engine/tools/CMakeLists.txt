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
target_include_directories(ptv_glb_optimize PRIVATE "${Stb_INCLUDE_DIR}")
set_target_properties(ptv_glb_optimize PROPERTIES FOLDER "ptv/tools")

# texture_compress: resize + linear-space mip chain + ASTC KTX2 (and a resized
# PNG/JPEG for PC VR, whose GPUs lack ASTC) for the asset_optimization stage.
if(Ktx_FOUND)
  add_executable(ptv_texture_compress texture_compress.cpp)
  target_link_libraries(ptv_texture_compress PRIVATE ptv_runtime KTX::ktx)
  target_include_directories(ptv_texture_compress PRIVATE "${Stb_INCLUDE_DIR}")
  set_target_properties(ptv_texture_compress PROPERTIES FOLDER "ptv/tools")
endif()

# glb_sockets: deterministic socket (muzzle / grip) derivation for the
# pipeline's asset_contract stage. Reuses the runtime's GLB merge so the
# geometry it measures is exactly what the engine will draw.
add_executable(ptv_glb_sockets glb_sockets.cpp)
target_link_libraries(ptv_glb_sockets PRIVATE ptv_runtime)
set_target_properties(ptv_glb_sockets PROPERTIES FOLDER "ptv/tools")

# glb_views: multi-view colour + triangle-id + depth renders of a GLB for the
# socket_generation stage (vision finds parts; ids map them onto the mesh).
# CPU rasteriser, so it runs on any build machine without a GPU.
add_executable(ptv_glb_views glb_views.cpp)
target_link_libraries(ptv_glb_views PRIVATE ptv_runtime)
target_include_directories(ptv_glb_views PRIVATE "${Stb_INCLUDE_DIR}")
set_target_properties(ptv_glb_views PROPERTIES FOLDER "ptv/tools")

# scene_views: whole-scene renders (colour + entity-id buffer) of a compiled
# scene.json for the scene_review stage. Same CPU rasteriser (cpu_raster.h).
add_executable(ptv_scene_views scene_views.cpp)
target_link_libraries(ptv_scene_views PRIVATE ptv_runtime)
target_include_directories(ptv_scene_views PRIVATE "${Stb_INCLUDE_DIR}")
set_target_properties(ptv_scene_views PROPERTIES FOLDER "ptv/tools")
