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

	private:
		Window& m_window;

		vk::Instance 				m_instance;
		vk::DispatchLoaderDynamic 	m_dispatch_loader;

		#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
			vk::DebugUtilsMessengerEXT m_debug_messenger;
		#endif
	};
}