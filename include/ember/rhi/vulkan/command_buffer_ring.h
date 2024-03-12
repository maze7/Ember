#pragma once

#include "core/platform.h"
#include "rhi/vulkan/render_device.h"

namespace Ember
{
    /**
     * Responsible for managing a number of CommandBuffers, generally this CommandBufferRing will allocate a CommandPool
     * for each frame * each render thread. Each pool will by reset after the corresponding frame has been rendererd.
     */
    class CommandBuffer;
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