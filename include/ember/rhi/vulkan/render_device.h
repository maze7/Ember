#pragma once

#include "rhi/rhi.h"
#include "core/pool.h"
#include "vulkan/vulkan.hpp"
#include "vk_mem_alloc.h"

namespace Ember
{
    // forward declarations
    struct Buffer;
    struct Shader;
    struct Texture;
    class Window;
    class CommandBuffer;
    class CommandBufferRing;

    class RenderDevice
    {
    public:
        static constexpr u32 k_max_frames_in_flight = 2;

        RenderDevice(Window& window);
        ~RenderDevice();

        u32 new_frame();
        void wait_idle();
        void present();
        void destroy();

        // Command Buffers
        void submit(CommandBuffer* cb, bool immediate = false);
        CommandBuffer* get_command_buffer(QueueType type = QueueType::Graphics, bool begin = false);
        CommandBuffer* get_command_buffer_instant();

        // Buffers
        Handle<Buffer> create_buffer(const BufferDef& def);
        Buffer* get_buffer(Handle<Buffer> handle);
        void destroy_buffer(Handle<Buffer> handle);

        // BindLayouts
        Handle<BindLayout> create_bind_layout(const BindLayoutDef& def);
        BindLayout* get_bind_layout(Handle<BindLayout> handle);
        void destroy_bind_layout(Handle<BindLayout> handle);

        // BindGroups
        Handle<BindGroup> create_bind_group(const BindGroupDef& def);
        BindGroup* get_bind_group(Handle<BindGroup> handle);

        // Create, Access and Destroy Shader (PSO) objects
        Handle<Shader> create_shader(const ShaderDef& def);
        Shader* get_shader(Handle<Shader> handle);
        void destroy_shader(Handle<Shader> handle);

        // Create, Access and Destroy texture objects
        Handle<Texture> create_texture(const TextureDef& def);
        Texture* get_texture(Handle<Texture> handle);
        void destroy_texture(Handle<Texture> handle);

        // TODO: This is temporary, RenderPasses will need to be abstracted.
        vk::RenderPass default_render_pass() const { return m_render_pass; }
        vk::Extent2D swapchain_extent() const { return m_swapchain_extent; }

    private:
        void create_swapchain();
        void destroy_swapchain();
        void create_framebuffers();
        void recreate_swapchain();

    private:
        VmaAllocator                    m_vma;
        Window&                         m_window;
        u32                             m_frame;
        u32                              m_swapchain_image;
        bool                            m_initialized = false;
        bool                            m_resized = false;

        vk::Instance                    m_instance;
        vk::Device                      m_device;
        vk::PhysicalDevice              m_physical_device;
        vk::Queue                       m_graphics_queue, m_present_queue;
        vk::DispatchLoaderDynamic       m_dispatch_loader;
        vk::RenderPass                  m_render_pass;
        vk::DescriptorPool              m_descriptor_pool;

        // Swapchain
        vk::SurfaceKHR                  m_surface;
        vk::SwapchainKHR                m_swapchain;
        vk::Format                      m_swapchain_format;
        vk::Extent2D                    m_swapchain_extent;
        std::vector<vk::Image>          m_swapchain_images;
        std::vector<vk::ImageView>      m_swapchain_image_views;
        std::vector<vk::Framebuffer>    m_swapchain_framebuffers;

        // Sync Objects (one for each frame in flight)
        std::vector<vk::Semaphore>		m_image_available_semaphores;
        std::vector<vk::Semaphore> 		m_render_finished_semaphores;
        std::vector<vk::Fence>			m_in_flight_fences;
        std::vector<CommandBuffer*>     m_pending_cmds;
        std::unique_ptr<CommandBufferRing>  m_cmd_ring;

        // GPU Resources
        Pool<Shader>                    m_shaders;
        Pool<Buffer>                    m_buffers;
        Pool<BindLayout>                m_bind_layouts;
        Pool<BindGroup>                 m_bind_groups;
        Pool<Texture>                   m_textures;

#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
        vk::DebugUtilsMessengerEXT      m_debug_messenger;
#endif
    };
}
