# Headless render tests (Linux only). Run tests/run-tests.sh.
add_executable(stagekit-render-test render-test.cpp)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
target_link_libraries(stagekit-render-test PRIVATE OBS::libobs Qt6::Widgets ${CMAKE_DL_LIBS})
set_target_properties(stagekit-render-test PROPERTIES CXX_STANDARD 17)

add_executable(stagekit-ambient-test ambient-test.cpp)
target_link_libraries(stagekit-ambient-test PRIVATE OBS::libobs Qt6::Widgets ${CMAKE_DL_LIBS})
set_target_properties(stagekit-ambient-test PROPERTIES CXX_STANDARD 17)

# What a scene collection costs a frame (tests/run-perf.sh); not run by run-tests.sh.
add_executable(collection-perf collection-perf.cpp)
target_link_libraries(collection-perf PRIVATE OBS::libobs ${CMAKE_DL_LIBS})
set_target_properties(collection-perf PROPERTIES CXX_STANDARD 17)

add_executable(stagekit-replacement-test replacement-test.cpp)
target_link_libraries(stagekit-replacement-test PRIVATE OBS::libobs Qt6::Widgets ${CMAKE_DL_LIBS})
set_target_properties(stagekit-replacement-test PROPERTIES CXX_STANDARD 17)

add_executable(
  stagekit-replacement-collection-test
  replacement-collection-test.cpp
  ../src/components/collection-convert.cpp
)
target_include_directories(stagekit-replacement-collection-test SYSTEM PRIVATE ../src/third-party/ports/json)
set_target_properties(stagekit-replacement-collection-test PROPERTIES CXX_STANDARD 17)

add_library(stagekit-replacement-probe MODULE replacement-probe.cpp)
target_link_libraries(stagekit-replacement-probe PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)
set_target_properties(stagekit-replacement-probe PROPERTIES PREFIX "" CXX_STANDARD 17)

# Load supplier and StageKit IDs in the same libobs process. These modules belong to the test build only.
execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/prepare-obs-ports.py" "${PROJECT_SOURCE_DIR}" "${port_build}"
    --reference
  COMMAND_ERROR_IS_FATAL ANY
)
foreach(key IN ITEMS move shader composite)
  set(ref_dir "${port_build}/reference/${key}")
  file(GLOB_RECURSE ref_files "${ref_dir}/*.c" "${ref_dir}/*.cpp")
  list(FILTER ref_files EXCLUDE REGEX "(nvidia-move-filter\\.c|move-directshow-filter\\.cpp)$")
  set(target "stagekit-test-original-${key}")
  add_library(${target} MODULE ${ref_files})
  target_link_libraries(${target} PRIVATE OBS::libobs OBS::obs-frontend-api)
  target_include_directories(${target} PRIVATE "${ref_dir}" "${ref_dir}/src")
  target_compile_options(${target} PRIVATE -w -Wno-error)
  if(key STREQUAL "move")
    set(orig_name "move-transition")
  elseif(key STREQUAL "shader")
    set(orig_name "obs-shaderfilter")
  else()
    set(orig_name "obs-composite-blur")
  endif()
  set_target_properties(
    ${target}
    PROPERTIES PREFIX "" OUTPUT_NAME "${orig_name}" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/obs-original"
  )
  add_dependencies(stagekit-replacement-test ${target})
endforeach()
