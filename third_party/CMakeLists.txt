add_subdirectory(fmt EXCLUDE_FROM_ALL)
add_subdirectory(stb EXCLUDE_FROM_ALL)
add_subdirectory(glm EXCLUDE_FROM_ALL)
add_subdirectory(unordered_dense EXCLUDE_FROM_ALL)

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
