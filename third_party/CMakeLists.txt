add_subdirectory(fmt EXCLUDE_FROM_ALL)
add_subdirectory(stb EXCLUDE_FROM_ALL)
add_subdirectory(glm EXCLUDE_FROM_ALL)
add_subdirectory(unordered_dense EXCLUDE_FROM_ALL)

# SDL3 is the desktop platform, private to ember::platform. Only the subsystems the platform
# uses are built: video, events and gamepads. Audio, rendering, cameras and the rest would
# otherwise compile, link and allocate for nothing.
if(EMBER_BUILD_PLATFORM)
	set(SDL_SHARED OFF CACHE BOOL "" FORCE)
	set(SDL_STATIC ON CACHE BOOL "" FORCE)
	set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
	set(SDL_TESTS OFF CACHE BOOL "" FORCE)
	set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

	foreach(subsystem AUDIO GPU RENDER CAMERA HAPTIC HIDAPI POWER SENSOR DIALOG TRAY NOTIFICATION)
		set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
	endforeach()
	set(SDL_VIDEO ON CACHE BOOL "" FORCE)
	set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)

	add_subdirectory(SDL EXCLUDE_FROM_ALL)
endif()

# Vulkan behind volk, which loads every entry point itself so nothing links libvulkan, and VMA for
# device memory. Both are private to ember::gpu.
if(EMBER_BUILD_GRAPHICS)
	set(VOLK_PULL_IN_VULKAN ON CACHE BOOL "" FORCE)
	add_subdirectory(volk EXCLUDE_FROM_ALL)
	add_subdirectory(vma EXCLUDE_FROM_ALL)
endif()

# Tracy profiling is only enabled when EMBER_ENABLE_PROFILING is ON
if (EMBER_ENABLE_PROFILING)
# Tracy's own CMake option must also be enabled.
	set(TRACY_ENABLE ON)
	set(TRACY_FIBERS ON)
	add_subdirectory(tracy EXCLUDE_FROM_ALL)

	# Tracy stamps the thread handle inside the client, so the
	# call has to survive optimisation.
	set_target_properties(TracyClient PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endif()
