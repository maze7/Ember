// The one translation unit that instantiates VulkanMemoryAllocator. Volk first, so VMA sees
// VK_NO_PROTOTYPES and fetches every entry point through the functions boot hands it. VMA's own
// warnings are its business, not the engine's.

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
