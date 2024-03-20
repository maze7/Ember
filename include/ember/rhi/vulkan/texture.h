#pragma once

#include "rhi/rhi.h"
#include "vulkan/vulkan.hpp"

namespace Ember
{
    struct Texture
    {
        vk::Image       image;
        vk::ImageView   view;
        vk::Format      format;
        vk::ImageLayout layout;
        VmaAllocation   allocation;
        u16             width = 1;
        u16             height = 1;
    };
}