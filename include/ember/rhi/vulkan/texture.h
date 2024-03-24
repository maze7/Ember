#pragma once

#include "rhi/rhi.h"
#include "vulkan/vulkan.hpp"

namespace Ember
{
	struct Sampler
	{
		vk::Sampler 			sampler;
		vk::Filter				min 		= vk::Filter::eNearest;
		vk::Filter				mag 		= vk::Filter::eNearest;
		vk::SamplerAddressMode 	u 			= vk::SamplerAddressMode::eRepeat;
		vk::SamplerAddressMode 	v 			= vk::SamplerAddressMode::eRepeat;
		vk::SamplerAddressMode 	w 			= vk::SamplerAddressMode::eRepeat;
	};

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