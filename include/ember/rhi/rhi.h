#pragma once

#ifdef EMBER_VULKAN
#   include "rhi/vulkan/buffer_vulkan.h"
#   include "rhi/vulkan/shader_vulkan.h"
#   include "rhi/vulkan/command_buffer_vulkan.h"
#   include "rhi/vulkan/render_device_vulkan.h"
#elif EMBER_DX12
    // not implemented yet
#endif