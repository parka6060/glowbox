cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" glowbox_version LIMIT_COUNT 1)
project(Glowbox VERSION ${glowbox_version} LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This build is for native Linux OBS. Use build.ps1 on Windows.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "The initial Linux package targets x86-64.")
endif()

find_package(libobs 32 REQUIRED CONFIG)
find_package(Threads REQUIRED)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(GLOB shader_sources CONFIGURE_DEPENDS "shaders/*.effect" "shaders/native/*.effect")
add_custom_command(
  OUTPUT "${generated}/compositor.effect" "${generated}/renderer_shader.hpp"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/build-support/assemble_shaders.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${generated}"
  DEPENDS ${shader_sources} shaders/manifest.txt build-support/assemble_shaders.py
  VERBATIM)
add_custom_target(glowbox_shaders DEPENDS "${generated}/compositor.effect" "${generated}/renderer_shader.hpp")

add_library(glowbox MODULE
  src/plugin.cpp

  src/compositor/compositor_graphics.cpp
  src/compositor/compositor_lifecycle.cpp
  src/compositor/compositor_properties.cpp
  src/compositor/native_renderer.cpp
  src/compositor/pipeline.cpp
  src/compositor/rendering.cpp
  src/compositor/settings.cpp
  src/compositor/shader_preparation.cpp
  src/compositor/source_participation.cpp

  src/scene/layers.cpp
  src/scene/lighting_geometry.cpp
  src/scene/mirror_scene.cpp

  src/sources/light_source.cpp
  src/sources/source_controls.cpp

  src/effects/leaf_canopy.cpp
  src/effects/reactive.cpp
  src/effects/rim_boundary.cpp
  src/effects/rim_model.cpp
  src/effects/tone.cpp

  src/integrations/move_compatibility.cpp
  "${generated}/renderer_shader.hpp")
add_dependencies(glowbox glowbox_shaders)
target_link_libraries(glowbox PRIVATE OBS::libobs Threads::Threads)
target_compile_features(glowbox PRIVATE cxx_std_17)
target_include_directories(glowbox PRIVATE "${generated}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(glowbox PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(glowbox PROPERTIES PREFIX "" OUTPUT_NAME "glowbox"
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_link_options(glowbox PRIVATE "LINKER:--no-undefined")

install(TARGETS glowbox LIBRARY DESTINATION glowbox/bin/64bit)
install(FILES "${generated}/compositor.effect" assets/loading.png assets/loading-failed.png assets/spinner.png DESTINATION glowbox/data)
install(FILES LICENSE DESTINATION glowbox)
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_FILE_NAME "Glowbox-Compositor-v${PROJECT_VERSION}-preview-linux-x86_64")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_CHECKSUM SHA256)
include(CPack)

