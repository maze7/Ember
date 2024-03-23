#pragma once

#include "core/platform.h"
#include "rhi/rhi.h"
#include "vk_mem_alloc.h"

#include <optional>
#include <vector>
#include <set>
#include <string_view>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_enums.hpp>

#define VK_CHECK(result) EMBER_ASSERT((result) == VK_SUCCESS)

namespace Ember
{
    struct QueueFamilyIndices
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

    // Utility function to return the queue indices for the given physical device
    inline auto queue_families(vk::PhysicalDevice gpu, vk::SurfaceKHR surface) {
        QueueFamilyIndices indices;
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

    // Utility function to return the swapchain support of a given physical device
    inline auto swapchain_support(vk::PhysicalDevice gpu, vk::SurfaceKHR surface) {
        return SwapchainSupport {
            .capabilities = gpu.getSurfaceCapabilitiesKHR(surface),
            .formats = gpu.getSurfaceFormatsKHR(surface),
            .present_modes = gpu.getSurfacePresentModesKHR(surface)
        };
    }

    // Utility function to ensure a physical device supports all requested extensions
    inline bool extensions_supported(vk::PhysicalDevice gpu, const std::vector<const char*>& required_extensions) {
        auto available_exts = gpu.enumerateDeviceExtensionProperties();
        std::set<std::string_view> required(required_extensions.begin(), required_extensions.end());

        // iteratively eliminate required extensions from the set
        for (const auto& ext : available_exts)
            required.erase(ext.extensionName);

        return required.empty();
    }

    // Utility function to convert an Ember RHI ShaderStage enum value to a vulkan shader stage
    static auto to_vk_shader_stage(ShaderStage value) {
        static constexpr vk::ShaderStageFlagBits vk_shader_stages[(u32)ShaderStage::Count] = {
            vk::ShaderStageFlagBits::eVertex,				// Vertex
            vk::ShaderStageFlagBits::eFragment,				// Fragment
            vk::ShaderStageFlagBits::eAll,					// All
        };

        return vk_shader_stages[(u32)value];
    }

    // Utility function to convert an Ember RHI BufferUsage enum value to a vulkan buffer usage value
    static auto to_vk_buffer_usage(BufferUsage value) {
        static constexpr VkBufferUsageFlags vk_buffer_usages[(u32)BufferUsage::Count] = {
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,				// Vertex
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,				// Index
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,				// Uniform
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,				// Staging
        };

        return vk_buffer_usages[(u32)value];
    }

    // Utility function to convert an Ember RHI Memory type enum value to a vulkan memory type
    static auto to_vk_memory_type(MemoryType value) {
        static constexpr VmaMemoryUsage vma_memory_usages[(u32)MemoryType::Count] = {
            VMA_MEMORY_USAGE_CPU_ONLY,						// CPU
            VMA_MEMORY_USAGE_GPU_ONLY,						// GPU
            VMA_MEMORY_USAGE_CPU_TO_GPU,					// CPU_GPU
            VMA_MEMORY_USAGE_AUTO,							// AUTO
        };

        return vma_memory_usages[(u32)value];
    }

    // Utility function to convert an Ember RHI vertex format to vulkan format
    static auto to_vk_vertex_format(Format value) {
        static constexpr vk::Format vk_vertex_formats[(u32)Format::Count] = {
            vk::Format::eR32Sfloat,							// Float
            vk::Format::eR32G32Sfloat,						// Float2
            vk::Format::eR32G32B32Sfloat,					// Float3
            vk::Format::eR32G32B32A32Sfloat,				// Float4
        };

        return vk_vertex_formats[(u32)value];
    }

    // Utility function to convert an Ember BindingType to a vulkan Descriptor binding type
    static auto to_vk_binding_type(BindingType value) {
        static constexpr vk::DescriptorType vk_binding_types[(u32)BindingType::Count] = {
            vk::DescriptorType::eUniformBuffer,
            vk::DescriptorType::eUniformBufferDynamic,
            vk::DescriptorType::eSampler,
            vk::DescriptorType::eCombinedImageSampler,
        };

        return vk_binding_types[(u32)value];
    };

    // Utility function to convert an Ember TextureType to a vulkan ImageType
    static auto to_vk_image_type(TextureType value) {
        static constexpr vk::ImageType vk_image_types[(u32)TextureType::Count] = {
            vk::ImageType::e1D,
            vk::ImageType::e2D,
            vk::ImageType::e3D,
        };

        return vk_image_types[(u32)value];
    }

    // Utility function to convert an Ember TextureType to a vulkan ImageViewType
    static auto to_vk_view_type(TextureType value) {
        static constexpr vk::ImageViewType vk_view_types[(u32)TextureType::Count] = {
            vk::ImageViewType::e1D,
            vk::ImageViewType::e2D,
            vk::ImageViewType::e3D,
        };

        return vk_view_types[(u32)value];
    }

    // Utility function to convert an Ember TextureFormat to a vulkan ImageFormat
    static auto to_vk_image_format(TextureFormat value) {
        static constexpr vk::Format vk_image_formats[(u32)TextureFormat::Count] = {
            vk::Format::eR8Srgb,
            vk::Format::eR8G8Srgb,
            vk::Format::eR8G8B8Srgb,
            vk::Format::eR8G8B8A8Srgb,
        };

        return vk_image_formats[(u32)value];
    }
}
