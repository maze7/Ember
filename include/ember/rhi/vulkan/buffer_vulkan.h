#pragma once

#include "vulkan/vulkan.h"
#include "vk_mem_alloc.h"

namespace Ember
{
    struct Buffer
    {
        VkBuffer                buffer;
        VkDeviceMemory          device_memory;
        VkDeviceSize            device_size;
        VkMemoryPropertyFlags   memory_flags;
        VmaAllocation           vma_allocation;
        BufferUsage             usage;
        MemoryType              memory_type;

        u64                     size = 0;
        u8*                     data = nullptr;
    };
}