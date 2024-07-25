#pragma once

#include "core/common.h"
#include <vulkan/vulkan.hpp>
#include <optional>
#include <vector>
#include <set>

#define VK_CHECK(result) EMBER_ASSERT((result) == VK_SUCCESS)

namespace Ember
{
	struct QueueFamilyInfices
	{
		std::optional<u32> graphics;
		std::optional<u32> present;

		[[nodiscard]] auto is_complete() const { return graphics.has_value() && present.has_value(); }
	};

	struct SwapchainSupport
	{
		vk::SurfaceCapabilitiesKHR capabilities;
		std::vector<vk::SurfaceFormatKHR> formats;
		std::vector<vk::PresentModeKHR> present_modes;
	};

	// utility function to return the queue indices for the given physical device
	auto queue_families(vk::PhysicalDevice gpu, vk::SurfaceKHR surface) {
		QueueFamilyInfices indices;
		auto families = gpu.getQueueFamilyProperties();

		for (int i = 0; i < families.size(); ++i) {
			if (families[i].queueFlags & vk::QueueFlagBits::eGraphics)
				indices.graphics = i;

			if (gpu.getSurfaceSupportKHR(i, surface))
				indices.present = i;

			if (indices.is_complete())
				break;
		}

		return indices;
	}

	// utility function to return the swapchain support of a given physical device
	auto swapchain_support(vk::PhysicalDevice gpu, vk::SurfaceKHR surface) {
		return SwapchainSupport {
			.capabilities = gpu.getSurfaceCapabilitiesKHR(surface),
		  	.formats = gpu.getSurfaceFormatsKHR(surface),
			.present_modes = gpu.getSurfacePresentModesKHR(surface)
		};
	}

	// utility function to ensure a physical device supports all requested extensions
	bool extensions_supported(vk::PhysicalDevice gpu, const std::vector<const char*>& required_extensions) {
		auto available_exts = gpu.enumerateDeviceExtensionProperties();
		std::set<std::string_view> required(required_extensions.begin() , required_extensions.end());

		// iteratively eliminate required extensions from the set
		for (const auto& ext : available_exts)
			required.erase(ext.extensionName);

		return required.empty();
	}
}