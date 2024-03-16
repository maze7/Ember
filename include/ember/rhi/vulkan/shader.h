#pragma once

#include "core/platform.h"
#include "vulkan/vulkan.hpp"

namespace Ember
{
    struct BindLayout
    {
        vk::DescriptorSetLayout layout;
        std::vector<vk::DescriptorSetLayoutBinding> bindings;
        u16 index;
    };

    struct BindGroup
    {
        vk::DescriptorSet descriptor_set;
    };

    struct Shader
    {
        Shader(vk::Pipeline pso_, vk::PipelineLayout layout_, vk::RenderPass pass_)
            : pso(pso_), layout(layout_), render_pass(pass_) {}

        vk::Pipeline        pso;
        vk::PipelineLayout  layout;
        vk::RenderPass      render_pass;
    };
}
