#pragma once

// Window operations needed by Vulkan. Opaque handle declarations keep Vulkan headers out of
// the platform backend and SDL headers out of the GPU module.

#include <ember/containers/span.h>
#include <ember/core/types.h>
#include <ember/platform/window.h>

typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;

static_assert(sizeof(void*) == 8, "a non-dispatchable Vulkan handle is a pointer only on 64 bit");

namespace ember::platform::vulkan
{
	// Loads the Vulkan library and returns vkGetInstanceProcAddr for the GPU module to cast.
	// Logs and returns null on failure.
	using ProcAddress = void (*)();
	ProcAddress load();

	// Balances load(). Once, at device shutdown.
	void unload();

	// Instance extensions a surface for our windows needs. Empty without a video system.
	Span<const char* const> instance_extensions();

	bool presentation_supported(VkInstance instance, VkPhysicalDevice device, u32 queue_family);

	// Caller owns the surface and must destroy it before the instance. Logs and returns null
	// for an invalid window or a platform failure.
	VkSurfaceKHR create_surface(WindowHandle window, VkInstance instance);
	void destroy_surface(VkInstance instance, VkSurfaceKHR surface);
}
