#pragma once

#ifdef EMBER_VULKAN
#   include "vulkan/buffer_vulkan.h"
#   include "vulkan/shader_vulkan.h"
#   include "vulkan/command_buffer_vulkan.h"
#   include "vulkan/render_device_vulkan.h"
#elif EMBER_DX12
    // not implemented yet
#endif