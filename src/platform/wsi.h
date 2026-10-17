#pragma once

// The window system functionality Vulkan requires. The GPU module calls these so that it never
// includes SDL (or any other platform backend), and the platform never includes Vulkan: the handle
// types are spelled here the way vulkan_core.h spells them, so a source that includes both sees one
// typedef twice, which C++ allows.

#include <ember/containers/span.h>
#include <ember/core/types.h>
#include <ember/platform/window.h>

typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;

static_assert(sizeof(void*) == 8, "a non-dispatchable Vulkan handle is a pointer only on 64 bit");

namespace ember::platform::vulkan
{
	// The loader's vkGetInstanceProcAddr, as a plain function pointer for the GPU module to
	// cast. Loads the Vulkan library the first time; null, logged, when there is none.
	using ProcAddress = void (*)();
	ProcAddress load();

	// Balances load(). Once, at device shutdown.
	void unload();

	// Instance extensions a surface for our windows needs. Empty without a video system.
	Span<const char* const> instance_extensions();

	bool presentation_supported(VkInstance instance, VkPhysicalDevice device, u32 queue_family);

	// A surface for [window], owned by the caller. Null, logged, when the window is gone or the
	// desktop refused.
	VkSurfaceKHR create_surface(WindowHandle window, VkInstance instance);
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface);
}
