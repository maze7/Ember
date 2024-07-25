#pragma once

#include <platform/window.h>
#include <vulkan/vulkan.hpp>

namespace Ember
{
	class RenderDevice
	{
	public:
		RenderDevice(Window& window);
		~RenderDevice();

		void destroy();
		void wait_idle();

	private:
		Window& m_window;

		vk::Instance 				m_instance;
		vk::DispatchLoaderDynamic 	m_dispatch_loader;
		vk::PhysicalDevice			m_physical_device;
		vk::Device					m_device;
		vk::Queue					m_graphics_queue, m_present_queue;
		vk::RenderPass				m_render_pass;
		vk::DescriptorPool			m_descriptor_pool;

		// Swapchain
		vk::SurfaceKHR				m_surface;
		vk::SwapchainKHR			m_swapchain;
		vk::Format					m_format;
		vk::Extent2D				m_swapchain_extent;
		std::vector<vk::Image>		m_swapchain_images;
		std::vector<vk::ImageView> 	m_swapchain_image_views;
		std::vector<vk::Framebuffer> m_swapchain_framebuffers;

		// Sync Objects (one for each frame in flight)
		std::vector<vk::Semaphore> 	m_image_available_semaphores;
		std::vector<vk::Semaphore> 	m_render_finished_semaphores;
		std::vector<vk::Fence>		m_in_flight_fences;

		#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
			vk::DebugUtilsMessengerEXT m_debug_messenger;
		#endif

		bool m_initialized = false;
	};
}