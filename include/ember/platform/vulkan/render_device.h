#pragma once

#include <platform/window.h>
#include <vulkan/vulkan.hpp>
#include "vma.h"

namespace Ember
{
	struct Buffer;
	struct Shader;
	struct Texture;
	struct Sampler;
	class Window;
	class CommandBuffer;
	class CommandBufferRing;

	class RenderDevice
	{
	public:
		static constexpr u32 k_max_frames_in_flight = 2;

		explicit RenderDevice(Window& window);
		~RenderDevice();

		void wait_idle();
		void destroy();

		// TODO: This is temporary, RenderPasses will need to be abstracted.
		[[nodiscard]] vk::RenderPass default_render_pass() const { return m_render_pass; }
		[[nodiscard]] vk::Extent2D swapchain_extent() const { return m_swapchain_extent; }

	private:
		void create_swapchain();
		void destroy_swapchain();
		void recreate_swapchain();
		void create_framebuffers();

	private:
		Window& 							m_window;
		VmaAllocator						m_vma;
		u32 								m_frame = 0;
		u32 								m_swapchain_image = 0;

		vk::Instance 						m_instance;
		vk::DispatchLoaderDynamic 			m_dispatch_loader;
		vk::PhysicalDevice					m_physical_device;
		vk::Device							m_device;
		vk::Queue							m_graphics_queue, m_present_queue;
		vk::RenderPass						m_render_pass;
		vk::DescriptorPool					m_descriptor_pool;

		// Swapchain
		vk::SurfaceKHR						m_surface;
		vk::SwapchainKHR					m_swapchain;
		vk::Format							m_swapchain_format;
		vk::Extent2D						m_swapchain_extent;
		std::vector<vk::Image>				m_swapchain_images;
		std::vector<vk::ImageView> 			m_swapchain_image_views;
		std::vector<vk::Framebuffer> 		m_swapchain_framebuffers;
		std::unique_ptr<CommandBufferRing> 	m_cmd_ring;

		// Sync Objects (one for each frame in flight)
		std::vector<vk::Semaphore> 			m_image_available_semaphores;
		std::vector<vk::Semaphore> 			m_render_finished_semaphores;
		std::vector<vk::Fence>				m_in_flight_fences;

		#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
			vk::DebugUtilsMessengerEXT 		m_debug_messenger;
		#endif

		bool 								m_initialized = false;
		bool								m_resized = false;
	};
}