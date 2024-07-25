#pragma once

#include <vulkan/vulkan.hpp>
#include "vma.h"

namespace Ember
{
	struct Buffer
	{
		vk::Buffer 				buffer;
		vk::DeviceMemory 		device_memory;
		vk::DeviceSize 			device_size;
		VkMemoryPropertyFlags 	memory_flags;
		VmaAllocation 			vma_allocation;
	};
}