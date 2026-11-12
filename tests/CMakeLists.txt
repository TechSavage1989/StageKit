# Headless render tests (Linux only). Run tests/run-tests.sh.
add_executable(stagekit-render-test render-test.cpp)
find_package(Qt6 REQUIRED COMPONENTS Widgets)
target_link_libraries(stagekit-render-test PRIVATE OBS::libobs Qt6::Widgets ${CMAKE_DL_LIBS})
set_target_properties(stagekit-render-test PROPERTIES CXX_STANDARD 17)
