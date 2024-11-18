cmake_minimum_required(VERSION 3.5)
project(Ember)

# c++ version & compiler flags
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O")
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
target_link_directories(Ember PUBLIC fmt::fmt)

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