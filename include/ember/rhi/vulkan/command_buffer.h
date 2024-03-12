#pragma once

#include "vulkan/vulkan.hpp"

namespace Ember
{
    class RenderDevice;
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
}