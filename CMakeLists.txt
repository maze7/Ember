cmake_minimum_required(VERSION 3.5)
project(Ember)

# c++ version & compiler flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Og")
set(CMAKE_CXX_FLAGS_PROFILE "${CMAKE_CXX_FLAGS_PROFILE} -O3")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# library configuration options
option(EMBER_USE_SDL3 "Whether Ember should use SDL3 for platform layer" ON)

# configure Ember library
add_library(
    Ember
    STATIC
    include/ember/core/common.h
    include/ember/core/logger.h
    src/core/logger.cpp
    include/ember/platform/window.h
    src/platform/window.cpp
    include/ember/input/axes.h
    include/ember/input/buttons.h
    include/ember/input/keys.h
    include/ember/input/mouse_buttons.h
    include/ember/input/keyboard.h
    include/ember/input/mouse.h
    include/ember/input/controller.h
    include/ember/input/input.h
    src/input/input.cpp
    src/input/keyboard.cpp
    include/ember/core/time.h
    src/core/time.cpp
    src/input/mouse.cpp
    src/input/controller.cpp
    include/ember/input/virtual_axis.h
    include/ember/input/virtual_button.h
    include/ember/input/virtual_stick.h
    src/input/virtual_axis.cpp
    src/input/virtual_button.cpp
    src/input/virtual_stick.cpp
    include/ember/math/math.h
    include/ember/core/pool.h
    include/ember/core/handle.h
    include/ember/graphics/render_device.h
    include/ember/graphics/texture.h
    include/ember/graphics/render_device_sdl.h
    src/graphics/render_device_sdl.cpp
    include/ember/graphics/shader.h
    src/graphics/shader.cpp
    src/graphics/render_device.cpp
    include/ember/graphics/texture_format.h
    src/graphics/texture.cpp
    include/ember/graphics/target.h
    include/ember/math/rect.h
    src/graphics/target.cpp
    include/ember/graphics/color.h
    src/graphics/color.cpp
    include/ember/graphics/vertex_type.h
    include/ember/graphics/draw_command.h
    include/ember/graphics/material.h
    include/ember/graphics/uniform_type.h
    src/graphics/material.cpp
    include/ember/core/hash.h
    src/graphics/draw_command.cpp
    include/ember/graphics/mesh.h
    include/ember/graphics/vertex_format.h
    include/ember/graphics/index_format.h
    src/graphics/mesh.cpp
    include/ember/graphics/batcher.h
    include/ember/graphics/blend.h
    src/graphics/batcher.cpp
    src/graphics/blend.cpp
    include/ember/math/quad.h
        include/ember/graphics/sub_texture.h
        src/graphics/sub_texture.cpp
)

# add include directories to Ember library
target_include_directories(
    Ember
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/ember>
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

if (EMBER_USE_SDL3)
    add_subdirectory(lib/sdl3)
    target_link_libraries(Ember PUBLIC SDL3::SDL3)
    target_include_directories(Ember PRIVATE "lib/sdl3/include")
endif()

# Ember uses fmt as a replacement for std::format while compiler support is still spotty.
add_subdirectory(lib/fmt)
target_include_directories(Ember PUBLIC lib/fmt/include)
target_link_libraries(Ember PUBLIC fmt::fmt)

# Ember uses GLM for all 3D math
add_subdirectory(lib/glm)
target_include_directories(Ember PUBLIC lib/glm)
target_link_libraries(Ember PUBLIC glm::glm)
target_compile_definitions(Ember PUBLIC GLM_ENABLE_EXPERIMENTAL)

# Ember uses stb_image.h to load images from disk
target_include_directories(Ember PUBLIC lib/stb)
target_sources(Ember PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib/stb/stb_image.cpp")

# compile definitions for different build configurations
target_compile_definitions(Ember PUBLIC
    $<$<CONFIG:Debug>:EMBER_DEBUG>
    $<$<CONFIG:Profile>:EMBER_PROFILE>
    $<$<CONFIG:Release>:EMBER_RELEASE>
)

# If building as a dependency of another project, export the library and include paths to PARENT_SCOPE
if (NOT PROJECT_IS_TOP_LEVEL)
    set(EMBER_LIBS Ember PARENT_SCOPE)
    set(EMBER_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include PARENT_SCOPE)
endif()