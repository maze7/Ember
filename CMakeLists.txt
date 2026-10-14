cmake_minimum_required(VERSION 4.3.0)
project(Ember VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(CMakeDependentOption)
include(cmake/ember_module.cmake)

option(EMBER_BUILD_PLATFORM "Build the platform module" ON)
option(EMBER_BUILD_GRAPHICS "Build the graphics module" ON)
option(EMBER_BUILD_AUDIO "Build the audio module" ON)
option(EMBER_ENABLE_PROFILING "Enable Tracy profiling in the engine" ON)
option(EMBER_USE_RPMALLOC "Back the general heap with rpmalloc" ON)

add_library(ember INTERFACE)
add_library(ember::ember ALIAS ember)

add_subdirectory(third_party)
add_subdirectory(src)
