// Compile VMA in one translation unit. Including Volk first disables Vulkan prototypes so VMA
// resolves entry points through the functions supplied at boot. Limit third-party warning
// suppression to this implementation include so engine code keeps its normal diagnostics.

#include <gpu/vulkan/common.h>

#if EMBER_COMPILER_GCC || EMBER_COMPILER_CLANG
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wunused-parameter"
	#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if EMBER_COMPILER_GCC || EMBER_COMPILER_CLANG
	#pragma GCC diagnostic pop
#endif
