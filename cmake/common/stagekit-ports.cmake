# Compile the pinned, source-available implementations as private StageKit modules.
# Each uses libobs's own module lifetime and a separate resource / translation directory.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(
  Qt6
  REQUIRED
  COMPONENTS Core Widgets Svg Xml Network
)
find_package(CURL REQUIRED)
if(UNIX AND NOT APPLE)
  find_package(Qt6 REQUIRED COMPONENTS Gui)
endif()

set(port_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/third-party/ports")
set(port_build "${CMAKE_CURRENT_BINARY_DIR}/stagekit-port-source")
file(GLOB_RECURSE port_inputs CONFIGURE_DEPENDS "${port_sources}/*")
set_property(
  DIRECTORY
  APPEND
  PROPERTY
    CMAKE_CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare-obs-ports.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/data/ports.json"
      ${port_inputs}
)
execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare-obs-ports.py" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${port_build}"
  COMMAND_ERROR_IS_FATAL ANY
)
file(READ "${port_sources}/origins.json" origins)
string(JSON port_count LENGTH "${origins}" projects)
math(EXPR port_last "${port_count} - 1")
foreach(index RANGE ${port_last})
  string(JSON key GET "${origins}" projects ${index} key)
  set(target "stagekit-port-${key}")
  set(folder "${port_build}/${key}")
  file(
    GLOB_RECURSE units
    "${folder}/*.c"
    "${folder}/*.cpp"
    "${folder}/*.cc"
    "${folder}/*.h"
    "${folder}/*.hpp"
    "${folder}/*.ui"
    "${folder}/*.qrc"
  )
  if(NOT WIN32)
    list(FILTER units EXCLUDE REGEX "(nvidia-move-filter\\.c|move-directshow-filter\\.cpp)$")
  endif()
  add_library(${target} MODULE ${units})
  target_include_directories(
    ${target}
    PRIVATE "${folder}" "${folder}/src" "${folder}/src/forms" "${folder}/lib/ndi" "${port_sources}/json"
  )
  target_link_libraries(${target} PRIVATE OBS::libobs OBS::obs-frontend-api)
  set_target_properties(
    ${target}
    PROPERTIES
      PREFIX ""
      C_STANDARD 17
      CXX_STANDARD 17
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/port-lib/$<CONFIG>"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stagekit-ports/$<CONFIG>"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stagekit-ports/$<CONFIG>"
  )
  if(key MATCHES "^(quick|dock|table|copy|collections|layouts|ndi|masks)$")
    target_link_libraries(${target} PRIVATE Qt6::Core Qt6::Widgets)
    set_target_properties(
      ${target}
      PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
    )
  endif()
  if(key STREQUAL "masks")
    target_link_libraries(${target} PRIVATE Qt6::Svg Qt6::Xml CURL::libcurl)
    if(WIN32)
      target_link_libraries(${target} PRIVATE crypt32)
    elseif(APPLE)
      target_link_libraries(${target} PRIVATE "-framework Security" "-framework CoreFoundation")
    endif()
  elseif(key STREQUAL "ndi")
    target_link_libraries(${target} PRIVATE Qt6::Network CURL::libcurl)
  elseif(key STREQUAL "dock" AND UNIX AND NOT APPLE)
    target_link_libraries(${target} PRIVATE Qt6::GuiPrivate)
  elseif(key STREQUAL "move" AND WIN32)
    target_link_libraries(${target} PRIVATE strmiids ws2_32)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W0 /WX-)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -w -Wno-error)
  endif()
  add_dependencies(stagekit ${target})
  if(APPLE)
    # libobs uses the entire macOS module filename as its registry name.
    set_target_properties(${target} PROPERTIES SUFFIX "")
    set(code_root "$<TARGET_BUNDLE_CONTENT_DIR:stagekit>/Frameworks")
    add_custom_command(
      TARGET stagekit
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${code_root}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>" "${code_root}/$<TARGET_FILE_NAME:${target}>"
      VERBATIM
    )
  else()
    if(WIN32)
      set(dest "stagekit/data/components/${key}")
    else()
      set(dest "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/stagekit/components/${key}")
    endif()
    install(TARGETS ${target} LIBRARY DESTINATION "${dest}" RUNTIME DESTINATION "${dest}")
    set(run_port "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/stagekit/components/${key}")
    add_custom_command(
      TARGET stagekit
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${run_port}"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${target}>" "${run_port}/$<TARGET_FILE_NAME:${target}>"
      VERBATIM
    )
  endif()
endforeach()
set(port_extension "${CMAKE_SHARED_MODULE_SUFFIX}")
if(APPLE)
  set(port_extension "")
endif()
target_compile_definitions(
  stagekit
  PRIVATE SK_PORT_EXTENSION="${port_extension}" SK_PORT_CONFIG="$<CONFIG>"
)
