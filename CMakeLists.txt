cmake_minimum_required(VERSION 3.5)
project(Ember)

# c++ version & compiler flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_PROFILE "${CMAKE_CXX_FLAGS_PROFILE} -O2")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# define library source files
add_library(
    Ember
    STATIC
    include/ember/core/logger.h
    src/core/logger.cpp
    include/ember/core/common.h
    include/ember/platform/window.h
    src/platform/window.cpp
)

# configure include directories
target_include_directories(
    Ember
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/Ember>
    PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

# define configuration options & default values
option(EMBER_USE_SDL2 "Compile with SDL2 dependency for platform module" ON)
option(EMBER_USE_VULKAN "Compile with Vulkan-based Render Hardware Interface (RHI)" ON)

# If SDL2 has been selected, include the SDL2 library
if (EMBER_USE_SDL2)
    find_package(SDL2 REQUIRED)
    target_compile_definitions(Ember PUBLIC SDL_MAIN_HANDLED EMBER_SDL2)
    target_include_directories(Ember PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(Ember PRIVATE ${SDL2_LIBRARIES})
endif()

# If Vulkan has been selected, include the Vulkan SDK
if (EMBER_USE_VULKAN)
    find_package(Vulkan REQUIRED)
    target_include_directories(Ember PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(Ember PUBLIC ${Vulkan_LIBRARIES})
    target_compile_definitions(Ember PUBLIC EMBER_VULKAN)
    target_sources(
        Ember
        PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ember/platform/vulkan/vma.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/vulkan/vma.cpp"
    )
endif()

# ember uses FMT as std::format is still not supported everywhere we want to target
add_subdirectory(lib/fmt)
target_include_directories(Ember PUBLIC lib/fmt/include)
target_link_libraries(Ember PUBLIC fmt::fmt)