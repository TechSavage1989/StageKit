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
  # Auto Capture reads a program's module list; the updater reads the list of
  # releases with Windows' own HTTP.
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE psapi winhttp)
endif()

# The release this build is: the newest notes file in docs/releases. The
# plugin's own version number stays the same through a run of pre-releases, so
# this is what the updater compares with what is on GitHub.
file(GLOB _release_notes RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/docs/releases" "docs/releases/*.md")
list(TRANSFORM _release_notes REPLACE "\\.md$" "")
list(SORT _release_notes COMPARE NATURAL)
list(GET _release_notes -1 _release)
# A finished version sorts before its own pre-releases by name; it is the newer.
string(REGEX REPLACE "-.*$" "" _release_base "${_release}")
if(_release_base IN_LIST _release_notes)
  set(_release "${_release_base}")
endif()
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE STAGEKIT_RELEASE="${_release}")

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
    src/common/hotkey-rename.hpp
    src/common/item-transition.cpp
    src/common/item-transition.hpp
    src/common/new-filter-settings.hpp
    src/common/saved-transform.hpp
    src/common/source-rename.cpp
    src/common/source-rename.hpp
    src/ambient/ambient-background.cpp
    src/ambient/ambient-background.hpp
    src/ambient/ambient-frame.cpp
    src/ambient/ambient-frame.hpp
    src/common/kawase-blur.cpp
    src/common/kawase-blur.hpp
    src/common/switch-ramp.cpp
    src/common/switch-ramp.hpp
    src/common/timing.cpp
    src/common/timing.hpp
    src/update/updater.cpp
    src/update/updater.hpp
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
    src/common/loading.cpp
    src/common/loading.hpp
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
    src/dock/window-layout.cpp
    src/dock/window-layout.hpp
    src/placement/scene-tools.cpp
    src/placement/scene-tools.hpp
    src/capture/app-capture.cpp
    src/capture/app-capture.hpp
    src/transition/morph.cpp
    src/transition/morph.hpp
    src/transition/morph-paint.cpp
    src/transition/morph-paint.hpp
    src/transition/item-morph.cpp
    src/transition/item-morph.hpp
    src/transition/fade-zoom.cpp
    src/transition/fade-zoom.hpp
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
    src/common/filter-order.hpp
    src/common/go-to-scene.cpp
    src/common/go-to-scene.hpp
    src/common/hid-items.cpp
    src/common/hid-items.hpp
    src/common/holds-hidden.cpp
    src/common/holds-hidden.hpp
    src/common/scene-filter-ids.hpp
    src/common/stand-in.hpp
    src/common/ui-task.cpp
    src/common/ui-task.hpp
    # ==== scene-filters:modes ====
    src/placement/scene-modes.cpp
    src/placement/scene-modes.hpp
    # ==== end scene-filters:modes ====
    # ==== scene-filters:transitions ====
    src/placement/scene-transitions.cpp
    src/placement/scene-transitions.hpp
    # ==== end scene-filters:transitions ====
    # ==== scene-filters:fit ====
    src/placement/fit-engine.cpp
    src/placement/fit-engine.hpp
    src/placement/scene-fit.cpp
    src/placement/scene-fit.hpp
    # ==== end scene-filters:fit ====
    # ==== scene-filters:standby-corners ====
    src/common/managed-filter.cpp
    src/common/managed-filter.hpp
    src/placement/scene-corners.cpp
    src/placement/scene-corners.hpp
    src/placement/scene-standby.cpp
    src/placement/scene-standby.hpp
    # ==== end scene-filters:standby-corners ====
    # ==== scene-filters:shadow ====
    src/look/look-paint.cpp
    src/look/look-paint.hpp
    src/look/outline-ambient.cpp
    src/look/outline-ambient.hpp
    src/look/outline-pattern.cpp
    src/look/outline-pattern.hpp
    src/look/scene-shadow.cpp
    src/look/scene-shadow.hpp
    # ==== end scene-filters:shadow ====
    # ==== scene-filters:blur ====
    src/blur/blur-pipeline.cpp
    src/blur/blur-pipeline.hpp
    src/look/scene-blur.cpp
    src/look/scene-blur.hpp
    # ==== end scene-filters:blur ====
    # ==== scene-filters:perf ====
    src/common/scene-rev.cpp
    src/common/scene-rev.hpp
    # ==== end scene-filters:perf ====
    src/third-party/obs-websocket-api.h
)

target_sources(
  stagekit
  PRIVATE
    src/components/components.cpp
    src/components/components.hpp
    src/components/classic-filters.cpp
    src/components/classic-filters.hpp
)
target_sources(
  stagekit
  PRIVATE
    src/components/collection-convert.cpp
    src/components/collection-convert.hpp
    src/components/collection-tools.cpp
    src/components/collection-tools.hpp
)
target_include_directories(stagekit SYSTEM PRIVATE src/third-party/ports/json)
include(cmake/common/stagekit-ports.cmake)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(STAGEKIT_BUILD_TESTS)
  add_subdirectory(tests)
elseif(WIN32)
  add_library(stagekit-replacement-probe MODULE EXCLUDE_FROM_ALL tests/replacement-probe.cpp)
  target_link_libraries(stagekit-replacement-probe PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Widgets)
  set_target_properties(stagekit-replacement-probe PROPERTIES PREFIX "" CXX_STANDARD 17)
endif()
