#pragma once

// Vulkan types and small translations shared by the backend. Include Volk first so it supplies
// VK_NO_PROTOTYPES and the function pointers; the engine loads Vulkan instead of linking it.

#include <volk.h>

#include <ember/core/log.h>
#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/gpu/types.h>

namespace ember::gpu::vk
{
	// Required features are core in 1.3, but their feature bits still need checking at boot.
	inline constexpr u32 API_VERSION = VK_API_VERSION_1_3;

	// Keep in TextureFormat order for direct lookup.
	inline constexpr VkFormat VK_FORMATS[] = {
	    VK_FORMAT_UNDEFINED,
	    VK_FORMAT_R8_UNORM,
	    VK_FORMAT_R8G8_UNORM,
	    VK_FORMAT_R8G8B8A8_UNORM,
	    VK_FORMAT_R8G8B8A8_SRGB,
	    VK_FORMAT_B8G8R8A8_UNORM,
	    VK_FORMAT_B8G8R8A8_SRGB,
	    VK_FORMAT_R16_SFLOAT,
	    VK_FORMAT_R16G16_SFLOAT,
	    VK_FORMAT_R16G16B16A16_SFLOAT,
	    VK_FORMAT_R32_SFLOAT,
	    VK_FORMAT_R32G32_SFLOAT,
	    VK_FORMAT_R32G32B32A32_SFLOAT,
	    VK_FORMAT_R32_UINT,
	    VK_FORMAT_B10G11R11_UFLOAT_PACK32,
	    VK_FORMAT_A2B10G10R10_UNORM_PACK32,
	    VK_FORMAT_D32_SFLOAT,
	    VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
	    VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
	    VK_FORMAT_BC3_UNORM_BLOCK,
	    VK_FORMAT_BC3_SRGB_BLOCK,
	    VK_FORMAT_BC4_UNORM_BLOCK,
	    VK_FORMAT_BC5_UNORM_BLOCK,
	    VK_FORMAT_BC6H_SFLOAT_BLOCK,
	    VK_FORMAT_BC7_UNORM_BLOCK,
	    VK_FORMAT_BC7_SRGB_BLOCK,
	};

	static_assert(sizeof(VK_FORMATS) / sizeof(VK_FORMATS[0]) == (u32)TextureFormat::Count,
	    "VK_FORMATS mirrors TextureFormat one to one");
	static_assert(VK_FORMATS[(u32)TextureFormat::D32Float] == VK_FORMAT_D32_SFLOAT);
	static_assert(VK_FORMATS[(u32)TextureFormat::BC7Srgb] == VK_FORMAT_BC7_SRGB_BLOCK);

	inline VkFormat to_vk(TextureFormat format)
	{
		return VK_FORMATS[(u32)format];
	}

	// Unknown native formats return Undefined.
	inline TextureFormat from_vk(VkFormat format)
	{
		for (u32 i = 1; i < (u32)TextureFormat::Count; ++i)
		{
			if (VK_FORMATS[i] == format)
			{
				return (TextureFormat)i;
			}
		}

		return TextureFormat::Undefined;
	}

	inline VkImageAspectFlags aspect_of(TextureFormat format)
	{
		return format_info(format).depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	}

	inline VkCompareOp to_vk(CompareOp op)
	{
		switch (op)
		{
			case CompareOp::Never:
				return VK_COMPARE_OP_NEVER;
			case CompareOp::Less:
				return VK_COMPARE_OP_LESS;
			case CompareOp::Equal:
				return VK_COMPARE_OP_EQUAL;
			case CompareOp::LessEqual:
				return VK_COMPARE_OP_LESS_OR_EQUAL;
			case CompareOp::Greater:
				return VK_COMPARE_OP_GREATER;
			case CompareOp::NotEqual:
				return VK_COMPARE_OP_NOT_EQUAL;
			case CompareOp::GreaterEqual:
				return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case CompareOp::Always:
			case CompareOp::Count:
				break;
		}

		return VK_COMPARE_OP_ALWAYS;
	}

	// Unlisted results return VK_RESULT_?; also log the number when the exact result matters.
	inline const char* result_name(VkResult result)
	{
		switch (result)
		{
			case VK_SUCCESS:
				return "VK_SUCCESS";
			case VK_NOT_READY:
				return "VK_NOT_READY";
			case VK_TIMEOUT:
				return "VK_TIMEOUT";
			case VK_INCOMPLETE:
				return "VK_INCOMPLETE";
			case VK_SUBOPTIMAL_KHR:
				return "VK_SUBOPTIMAL_KHR";
			case VK_ERROR_OUT_OF_HOST_MEMORY:
				return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY:
				return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED:
				return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST:
				return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_MEMORY_MAP_FAILED:
				return "VK_ERROR_MEMORY_MAP_FAILED";
			case VK_ERROR_LAYER_NOT_PRESENT:
				return "VK_ERROR_LAYER_NOT_PRESENT";
			case VK_ERROR_EXTENSION_NOT_PRESENT:
				return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT:
				return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER:
				return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_TOO_MANY_OBJECTS:
				return "VK_ERROR_TOO_MANY_OBJECTS";
			case VK_ERROR_FORMAT_NOT_SUPPORTED:
				return "VK_ERROR_FORMAT_NOT_SUPPORTED";
			case VK_ERROR_SURFACE_LOST_KHR:
				return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
				return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR:
				return "VK_ERROR_OUT_OF_DATE_KHR";
			case VK_ERROR_VALIDATION_FAILED_EXT:
				return "VK_ERROR_VALIDATION_FAILED_EXT";
			case VK_ERROR_OUT_OF_POOL_MEMORY:
				return "VK_ERROR_OUT_OF_POOL_MEMORY";
			case VK_ERROR_FRAGMENTED_POOL:
				return "VK_ERROR_FRAGMENTED_POOL";
			case VK_ERROR_UNKNOWN:
				return "VK_ERROR_UNKNOWN";
			default:
				return "VK_RESULT_?";
		}
	}
}

// Log and assert when a call expected to succeed fails. Runtime failures such as device creation,
// submission, or presentation need their own result handling instead of this check.
#define EMBER_VK_CHECK(expr)                                                                       \
	do                                                                                             \
	{                                                                                              \
		const VkResult ember_vk_result = (expr);                                                   \
		if (ember_vk_result != VK_SUCCESS)                                                         \
		{                                                                                          \
			EMBER_ERROR("vulkan: {} failed: {} ({})", #expr,                                       \
			    ::ember::gpu::vk::result_name(ember_vk_result), (int)ember_vk_result);             \
			EMBER_ASSERT_MSG(false, "a Vulkan call failed");                                       \
		}                                                                                          \
	} while (false)
