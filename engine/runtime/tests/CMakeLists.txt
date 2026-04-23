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
