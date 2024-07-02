#include <SDL2/SDL_vulkan.h>
#include "platform/vulkan/render_device.h"

#ifndef EMBER_APP_VERSION
	#define EMBER_APP_VERSION 1
#endif

using namespace Ember;

namespace
{
	// vulkan validation layers that should be enabled on the instance
	const std::vector<const char*> validation_layers = {
		#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
			"VK_LAYER_KHRONOS_validation",
		#endif
	};

	// vulkan extensions that shuld be enabled on the logical device
	const std::vector<const char*> device_extensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		#if defined(__APPLE__)
			"VK_KHR_portability_subset",
		#endif
	};
}

// debug message callback used by vulkan validation layers, forwards to the ember logger
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
	vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
	vk::DebugUtilsMessageTypeFlagBitsEXT type,
	const vk::DebugUtilsMessengerCallbackDataEXT* callback_data,
	void* user_data
) {
	// log warnings, break on errors
	if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
	{
		Logger::warn("{}", callback_data->pMessage);
	}
	else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
	{
		Logger::error("{}", callback_data->pMessage);
		EMBER_DEBUG_BREAK();
	}

	return VK_FALSE;
}

// utility function to create a debug messenger
auto debug_messenger_create_info()
{
	using MessageType = vk::DebugUtilsMessageTypeFlagBitsEXT;
	using SeverityFlag = vk::DebugUtilsMessageSeverityFlagBitsEXT;

	return vk::DebugUtilsMessengerCreateInfoEXT
	{
		{},
		SeverityFlag::eError | SeverityFlag::eWarning,
		MessageType::eGeneral | MessageType::eValidation | MessageType::ePerformance,
		reinterpret_cast<PFN_vkDebugUtilsMessengerCallbackEXT>(debug_callback)
	};
}

RenderDevice::RenderDevice(Window &window) : m_window(window)
{
	// get the list of all vulkan extensions required by SDL2
	u32 num_sdl_ext = 0;
	SDL_Vulkan_GetInstanceExtensions(window.native_handle(), &num_sdl_ext, nullptr);
	std::vector<const char*> extensions(num_sdl_ext);
	SDL_Vulkan_GetInstanceExtensions(window.native_handle(), &num_sdl_ext, extensions.data());

	// on apple, we need the portability extensions enabled for MoltenVK to work
	#if defined(__APPLE__)
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	#endif

	vk::ApplicationInfo app_info(window.title(), EMBER_APP_VERSION, "Ember", 1, VK_API_VERSION_1_3);
	vk::InstanceCreateInfo instance_info({}, &app_info, {}, extensions);

	// configure validation layers for EMBER_DEBUG and EMBER_PROFILE builds
	#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
		// check if all required validation layers are supported, and if so enable them
		auto available_layers = vk::enumerateInstanceLayerProperties();
		for (const char* layer_name : validation_layers)
		{
			bool found = false;
			for (const auto& layer : available_layers)
			{
				if (std::string_view(layer_name) == std::string_view(layer.layerName))
				{
					found = true;
					break;
				}
			}

			EMBER_ASSERT(found);
		}

		const auto messenger_info = debug_messenger_create_info();
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		instance_info = vk::InstanceCreateInfo({}, &app_info, validation_layers, extensions);

		const vk::ValidationFeatureEnableEXT validation_features[] = {
			vk::ValidationFeatureEnableEXT::eGpuAssisted,
			vk::ValidationFeatureEnableEXT::eSynchronizationValidation,
		};

		vk::ValidationFeaturesEXT features(validation_features, {}, &messenger_info);
		instance_info.pNext = &features;

		// log the enabled vulkan extensions if we're running in debug mode
		Logger::log("Enabled vulkan extensions: ");
		for (auto& ext : extensions)
			Logger::log("\t{}", ext);
	#endif

	// on Apple devices we need to enable the portability bit on the instance flags
	#if defined(__APPLE__)
		instance_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
	#endif
}

RenderDevice::~RenderDevice()
{

}