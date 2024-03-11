#include <core/platform.h>
#include "vulkan/vulkan.hpp"
#include "rhi/vulkan/command_buffer.h"

using namespace Ember;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// COMMAND BUFFER
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CommandBuffer::begin() {
    // Ensure we haven't already begun recording to this command buffer
    EMBER_ASSERT(!m_recording);

    m_cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
    m_recording = true;

    // TODO: For now we begin the render pass here. Render passes should probably be abstracted and moved to some form of
    // begin_pass() and end_pass() functions within the command buffer. This way we can submit multiple passes at once.
    vk::RenderPassBeginInfo begin_info(m_pass, m_framebuffer, vk::Rect2D({0, 0}, m_gpu->swapchain_extent()), m_clears);
    m_cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
}

void CommandBuffer::end() {
    EMBER_ASSERT(m_recording);

    m_cmd.endRenderPass();
    m_cmd.end();
    m_recording = false;
}

void CommandBuffer::reset() {
    m_recording = false;
    m_cmd.reset();
}

void CommandBuffer::set_viewport(i32 x, i32 y, i32 width, i32 height) {
    m_cmd.setViewport(0, vk::Viewport(x, y, width, height, 0, 1));
}

void CommandBuffer::set_scissor(i32 x, i32 y, i32 width, i32 height) {
    m_cmd.setScissor(0, vk::Rect2D({x, y}, { (u32)width, (u32)height }));
}

void CommandBuffer::clear(f32 red, f32 green, f32 blue, f32 alpha) {
    m_clears[0].color = { red, green, blue, alpha };
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// COMMAND BUFFER RING
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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
    return cb;
}