ember_add_module(core
	SOURCES
		core/assert.cpp
		core/log.cpp
		core/thread_posix.cpp
		core/thread_win32.cpp
		core/time.cpp
		core/frame.cpp
		core/filesystem.cpp
		core/filesystem_posix.cpp
		core/filesystem_win32.cpp
		core/job.cpp
		core/job_scheduler.cpp
		core/io_thread.cpp
		core/fiber_posix.cpp
		core/fiber_aarch64_aapcs.S
		core/fiber_x86_64_sysv.S
		memory/tlsf.c
		memory/arena.cpp
		memory/block_heap.cpp
		memory/heap.cpp
		memory/memory.cpp
		memory/tracker.cpp
		memory/virtual_memory_posix.cpp
		memory/virtual_memory_win32.cpp
	PUBLIC_DEPS
		fmt::fmt
		glm::glm
		unordered_dense::unordered_dense
		$<$<BOOL:${EMBER_ENABLE_PROFILING}>:Tracy::TracyClient>
	PRIVATE_DEPS
		$<$<PLATFORM_ID:Windows>:shell32>
		$<$<PLATFORM_ID:Windows>:ole32>
)

if(EMBER_BUILD_PLATFORM)
	ember_add_module(platform
		SOURCES
			platform/input_state.cpp
			platform/platform_sdl3.cpp
		PUBLIC_DEPS
			ember::core
		PRIVATE_DEPS
			SDL3::SDL3-static
	)
endif()

if(EMBER_BUILD_GRAPHICS)
	ember_add_module(gpu
		SOURCES
			gpu/types.cpp
			gpu/vulkan/boot.cpp
			gpu/vulkan/buffer.cpp
			gpu/vulkan/commands.cpp
			gpu/vulkan/frame.cpp
			gpu/vulkan/heap.cpp
			gpu/vulkan/pipeline.cpp
			gpu/vulkan/sampler.cpp
			gpu/vulkan/swapchain.cpp
			gpu/vulkan/texture.cpp
			gpu/vulkan/transient.cpp
			gpu/vulkan/vma.cpp
		PUBLIC_DEPS
			ember::core
			$<$<BOOL:${EMBER_BUILD_PLATFORM}>:ember::platform>
		PRIVATE_DEPS
			volk::volk
			GPUOpen::VulkanMemoryAllocator
	)

	# Volk loads every entry point itself, so no backend source may see
	# the prototypes, and VMA fetches its through volk's tables. Vulkan
	# structs are built with designated initializers that leave their
	# zero fields out on purpose.
	target_compile_definitions(ember_gpu PRIVATE
		VK_NO_PROTOTYPES
		VMA_STATIC_VULKAN_FUNCTIONS=0
		VMA_DYNAMIC_VULKAN_FUNCTIONS=1
	)
	target_compile_options(ember_gpu PRIVATE
		$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>
	)
endif()
