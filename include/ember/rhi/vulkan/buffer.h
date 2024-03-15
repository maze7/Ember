#pragma once

#include "rhi/rhi.h"
#include "vulkan/vulkan.hpp"
#include "vk_mem_alloc.h"
#include <vulkan/vulkan_enums.hpp>

namespace Ember
{
    struct Buffer
    {
        vk::Buffer              buffer;
        vk::DeviceMemory        device_memory;
        vk::DeviceSize          device_size;
        VkMemoryPropertyFlags   memory_flags;
        VmaAllocation           vma_allocation;
        BufferUsage             usage;
        MemoryType              memory_type;

        u64                     size = 0;
        u8*                     data = nullptr;
        i64                     parent = -1;
    };
}
