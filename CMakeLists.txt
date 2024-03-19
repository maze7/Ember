cmake_minimum_required(VERSION 3.5)
project(Ember)

# C++ version & Compiler flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_PROFILE "${CMAKE_CXX_FLAGS_PROFILE} -O2")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# Define library source files
add_library(Ember
    STATIC
        src/core/log.cpp
        include/ember/core/log.h
        include/ember/core/platform.h
        include/ember/rhi/rhi.h
        include/ember/system/window.h
        src/system/window.cpp
        include/ember/ember.h
        include/ember/core/pool.h
)

# Configure Include directories
target_include_directories(Ember
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/ember>
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# Define configuration options & default values
option(EMBER_USE_VULKAN "Compile with Vulkan implementation for RHI" ON)

# Ember uses SDL2 for platform abstraction (Primarily for Windows & Input)
find_package(SDL2 REQUIRED)
target_compile_definitions(Ember PUBLIC SDL_MAIN_HANDLED EMBER_SDL2)
target_include_directories(Ember PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(Ember PRIVATE ${SDL2_LIBRARIES})

# If Vulkan has been selected, include the Vulkan SDK
if (EMBER_USE_VULKAN)
    find_package(Vulkan REQUIRED)
    target_include_directories(Ember PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(Ember PUBLIC ${Vulkan_LIBRARIES})
    target_compile_definitions(Ember PUBLIC EMBER_VULKAN)
    target_sources(Ember
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/vk_mem_alloc.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/vulkan/vk_mem_alloc.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/buffer.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/command_buffer.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/command_buffer_ring.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/vulkan/command_buffer.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/vulkan/command_buffer_ring.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/render_device.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/vulkan/render_device.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/shader.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/rhi/vulkan/vulkan_util.h"
    )
endif()

# Ember uses FMT as std::format is still not supported everywhere we want to target
add_subdirectory(lib/fmt)
target_include_directories(Ember PUBLIC lib/fmt/include)
target_link_libraries(Ember PUBLIC fmt::fmt)

# For now, Ember uses GLM for all 3D math
add_subdirectory(lib/glm)
target_include_directories(Ember PUBLIC lib/glm/glm)
target_link_libraries(Ember PUBLIC glm::glm)

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