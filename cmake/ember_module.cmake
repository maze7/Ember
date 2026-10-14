# Warning flags shared by every module. Linked PRIVATE so they never leak to
# the projects that consume the engine.
add_library(ember_build_options INTERFACE)
target_compile_options(ember_build_options INTERFACE
	$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
	$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->
)

# ember_add_module(<name>
#	[SOURCES <path>...]        relative to src
#	[PUBLIC_DEPS <target>...]
#	[PRIVATE_DEPS <target>...])
#
# Every module exports the whole of include/ember. Which headers belong to
# which module is a naming convention, not something CMake tracks, so a
# module may draw its sources from any number of directories under src.
# A module with no SOURCES is header-only.
function(ember_add_module name)
	cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

	set(target ember_${name})
	list(TRANSFORM ARG_SOURCES PREPEND ${Ember_SOURCE_DIR}/src/)

	if(ARG_SOURCES)
		add_library(${target} STATIC ${ARG_SOURCES})
		set(scope PUBLIC)
	else()
		add_library(${target} INTERFACE)
		set(scope INTERFACE)
	endif()
	add_library(ember::${name} ALIAS ${target})

	target_include_directories(${target} ${scope}
		$<BUILD_INTERFACE:${Ember_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	)

	target_compile_features(${target} ${scope} cxx_std_20)
	target_link_libraries(${target} ${scope} ${ARG_PUBLIC_DEPS})

	string(TOUPPER ${name} NAME)
	target_compile_definitions(${target} ${scope} EMBER_HAS_${NAME}=1)

	if(ARG_SOURCES)
		target_include_directories(${target} PRIVATE ${Ember_SOURCE_DIR}/src)
		target_link_libraries(${target}
			PRIVATE ${ARG_PRIVATE_DEPS} ember_build_options
		)

		# Ember has no C++20 module units, so don't scan for them. The scan
		# adds a build step and puts GCC-only flags into every compile
		# command, which breaks every tool that reads the database.
		set_target_properties(${target} PROPERTIES
			CXX_EXTENSIONS OFF
			CXX_SCAN_FOR_MODULES OFF
		)
	endif()

	target_link_libraries(ember INTERFACE ember::${name})
endfunction()
