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

add_executable(stagekit-replacement-collection-test replacement-collection-test.cpp ../src/components/collection-convert.cpp)
target_include_directories(stagekit-replacement-collection-test SYSTEM PRIVATE ../src/third-party/ports/json)
set_target_properties(stagekit-replacement-collection-test PROPERTIES CXX_STANDARD 17)

add_library(stagekit-replacement-probe MODULE replacement-probe.cpp)
target_link_libraries(stagekit-replacement-probe PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)
set_target_properties(stagekit-replacement-probe PROPERTIES PREFIX "" CXX_STANDARD 17)
