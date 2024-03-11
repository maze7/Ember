#pragma once

#include "rhi/vulkan/render_device.h"
#include "vulkan/vulkan.hpp"

namespace Ember
{
    class CommandBufferRing;

    /**
     * Abstraction above a single device command buffer.
     */
    class CommandBuffer
    {
    protected:
        // Only the RenderDevice should be able to create a CommandBuffer
        CommandBuffer() = default;

    public:
        void begin();
        void end();
        void reset();

        void set_viewport(i32 x, i32 y, i32 width, i32 height);
        void set_scissor(i32 x, i32 y, i32 width, i32 height);
        void clear(f32 red, f32 green, f32 blue, f32 alpha);

    private:
        friend class RenderDevice;
        friend class CommandBufferRing;

        vk::CommandBuffer m_cmd;
        vk::Framebuffer m_framebuffer;
        vk::RenderPass m_pass;
        vk::ClearValue m_clears[2]; // 0 = color, 1 = depth;

        RenderDevice* m_gpu;
        bool m_recording = false;
        u32 m_image_index = 0;
    };

    /**
     * Responsible for managing a number of CommandBuffers, generally this CommandBufferRing will allocate a CommandPool
     * for each frame * each render thread. Each pool will by reset after the corresponding frame has been rendererd.
     */
    class CommandBufferRing
    {
    protected:
        static constexpr u16 k_max_threads = 1;
        static constexpr u16 k_max_pools = RenderDevice::k_max_frames_in_flight * k_max_threads;
        static constexpr u16 k_buffers_per_pool = 4;
        static constexpr u16 k_max_buffers = k_buffers_per_pool * k_max_pools;

    public:
        CommandBufferRing() = default;
        ~CommandBufferRing();

        void init(RenderDevice* gpu, vk::Device device, u32 queue_index);
        void destroy();
        void reset_pools(u32 frame);

        CommandBuffer* get_command_buffer(u32 frame, bool begin);
        CommandBuffer* get_command_buffer_instant(u32 frame, bool begin);

    private:
        friend class RenderDevice;

        // shorthand to calculate the corresponding CommandPool for a given CommandBuffer index
        static u16 pool_from_index(u32 index) { return (u16)index / k_buffers_per_pool; }

        RenderDevice*   m_gpu;
        vk::Device      m_device;
        vk::CommandPool m_command_pools[k_max_pools];
        CommandBuffer   m_command_buffers[k_max_buffers];
        u8              m_next_free[k_max_pools];
        bool            m_initialized = false;
    };
}