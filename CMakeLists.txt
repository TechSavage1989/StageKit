cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(STAGEKIT_BUILD_TESTS "Build the headless render tests (Linux only)" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
if(WIN32)
  # Auto Capture reads a program's module list.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE psapi)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/common/easing.cpp
    src/common/easing.hpp
    src/ambient/ambient-background.cpp
    src/ambient/ambient-background.hpp
    src/common/kawase-blur.cpp
    src/common/kawase-blur.hpp
    src/common/switch-ramp.cpp
    src/common/switch-ramp.hpp
    src/view/cropped-view.cpp
    src/view/cropped-view.hpp
    src/zoom/ramp-zoom.cpp
    src/zoom/ramp-zoom.hpp
    src/capture/auto-capture.cpp
    src/capture/auto-capture.hpp
    src/capture/active-app.cpp
    src/capture/active-app.hpp
    src/capture/program-trigger.cpp
    src/capture/program-trigger.hpp
    src/capture/standby.cpp
    src/capture/standby.hpp
    src/common/pick-list.cpp
    src/common/pick-list.hpp
    src/common/scene-util.hpp
    src/common/shown-scale.cpp
    src/common/shown-scale.hpp
    src/common/start-image.cpp
    src/common/start-image.hpp
    src/dock/arrange.cpp
    src/dock/arrange.hpp
    src/dock/arrange-tools.cpp
    src/dock/arrange-tools.hpp
    src/dock/preview-guides.cpp
    src/dock/preview-guides.hpp
    src/dock/stagekit-dock.cpp
    src/dock/stagekit-dock.hpp
    src/placement/scene-tools.cpp
    src/placement/scene-tools.hpp
    src/placement/fit-item.cpp
    src/placement/fit-item.hpp
    src/placement/visibility-link.cpp
    src/placement/visibility-link.hpp
    src/frame/auto-frame.cpp
    src/frame/auto-frame.hpp
    src/placement/place-item.cpp
    src/placement/place-item.hpp
    src/placement/show-in-scenes.cpp
    src/placement/show-in-scenes.hpp
    src/shape/corners.cpp
    src/shape/corners.hpp
    src/shadow/shadow-backdrop.cpp
    src/shadow/shadow-backdrop.hpp
    src/blur/ramp-blur.cpp
    src/blur/ramp-blur.hpp
    src/api/vendor-api.cpp
    src/api/vendor-api.hpp
    src/third-party/obs-websocket-api.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(STAGEKIT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
