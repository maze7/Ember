#include "rhi/vulkan/command_buffer_ring.h"
#include "rhi/vulkan/command_buffer.h"

using namespace Ember;

void CommandBufferRing::init(RenderDevice *gpu, vk::Device device, u32 queue_index) {
    m_gpu = gpu;
    m_device = device;

    // allocate command pools
    for (auto& command_pool : m_command_pools) {
        vk::CommandPoolCreateInfo cmd_pool_info(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, queue_index);
        command_pool = m_device.createCommandPool(cmd_pool_info);
    }

    // allocate command buffers
    for (u32 i = 0; i < k_max_buffers; i++) {
        vk::CommandBufferAllocateInfo buffer_info(m_command_pools[pool_from_index(i)], vk::CommandBufferLevel::ePrimary, 1);
        m_command_buffers[i].m_cmd = device.allocateCommandBuffers(buffer_info)[0];
        m_command_buffers[i].m_gpu = gpu;
        m_command_buffers[i].reset();
    }

    m_initialized = true;
}

void CommandBufferRing::destroy() {
    for (auto m_command_pool : m_command_pools) {
        m_device.destroyCommandPool(m_command_pool);
    }

    m_initialized = false;
}

void CommandBufferRing::reset_pools(u32 frame_index) {
    for (u32 i = 0; i < k_max_threads; i++) {
        m_device.resetCommandPool(m_command_pools[frame_index * k_max_threads + i]);
    }
}

CommandBufferRing::~CommandBufferRing() {
    // only try to destroy device objects if destroy() hasn't been called manually.
    if (m_initialized) destroy();
}

CommandBuffer* CommandBufferRing::get_command_buffer(u32 frame, bool begin) {
    auto cb = &m_command_buffers[frame * k_buffers_per_pool];

    if (begin) {
        cb->begin();
    }

    return cb;
}

CommandBuffer* CommandBufferRing::get_command_buffer_instant(u32 frame, bool begin) {
    auto cb = &m_command_buffers[frame * k_buffers_per_pool + 1];

    Log::trace("frame: {}, k_buffers_per_pool: {}, cmd: {}", frame, k_buffers_per_pool, frame * k_buffers_per_pool + 1);

    if (begin) {
        cb->m_cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    }

    return cb;
}