#include "core/platform.h"
#include "core/log.h"
#include "system/window.h"
#include "rhi/vulkan/render_device.h"
#include "rhi/vulkan/command_buffer.h"
#include "rhi/vulkan/vulkan_util.h"
#include <SDL2/SDL_vulkan.h>
#include <vector>
#include <optional>
#include <SDL2/SDL_events.h>

using namespace Ember;

namespace
{
    // vulkan validation layers that should be enabled on the instance
    const std::vector<const char*> validation_layers = {
        #if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
            "VK_LAYER_KHRONOS_validation",
        #endif
    };

    // vulkan extensions that should be enabled on the logical device
    const std::vector<const char*> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        #ifdef __APPLE__
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
    // Log warnings, break on errors
    if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) {
        Log::warn("{}", callback_data->pMessage);
    } else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) {
        Log::error("{}", callback_data->pMessage);
        EMBER_DEBUG_BREAK();
    }

    return VK_FALSE;
}


// utility function to create a debug messenger
vk::DebugUtilsMessengerCreateInfoEXT debug_messenger_create_info() {
    using MessageType = vk::DebugUtilsMessageTypeFlagBitsEXT;

    return {
        {},
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
        MessageType::eGeneral | MessageType::eValidation | MessageType::ePerformance,
        reinterpret_cast<PFN_vkDebugUtilsMessengerCallbackEXT>(debug_callback)
    };
}

RenderDevice::RenderDevice(Ember::Window& window) : m_window(window) {
    // get the list of all vulkan extensions required by SDL2
    u32 num_sdl_ext = 0;
    SDL_Vulkan_GetInstanceExtensions(window.native_handle(), &num_sdl_ext, nullptr);
    std::vector<const char*> extensions(num_sdl_ext);
    SDL_Vulkan_GetInstanceExtensions(window.native_handle(), &num_sdl_ext, extensions.data());

    // on apple, we need the portability extensions enabled for MoltenVK to work
#ifdef __APPLE__
    // add portability extensions
	extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif

    vk::ApplicationInfo app_info("Ember", 1, "Ember", 1, VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instance_info({}, &app_info, {}, extensions);

    // configure validations layers for DEBUG and PROFILE builds
#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
    // check if all required validation layers are supported, and if so enable them
    auto available_layers = vk::enumerateInstanceLayerProperties();
    for (const char* layer_name : validation_layers) {
        bool found = false;
        // search each available layer for the current required layer, break early if we find it
        for (const auto& layer : available_layers) {
            if (std::string_view(layer_name) == std::string_view(layer.layerName)) {
                found = true;
                break;
            }
        }

        // If a layer is missing, we can just throw. We're dealing with debug builds, so we don't need to handle gracefully
        if (!found)
            throw std::runtime_error(std::string("failed to find required validation layer: ") + layer_name);
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
    Log::trace("Enabled vulkan extensions: ");
    for (auto& ext : extensions) {
        Log::trace("\t{}", ext);
    }
#endif

    // on Apple devices we need to enable the portability bit on the instance flags
#ifdef __APPLE__
    instance_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif

    // create vulkan instance, dispatch loader and debug messenger
    m_instance = vk::createInstance(instance_info);
    m_dispatch_loader = vk::DispatchLoaderDynamic(m_instance, vkGetInstanceProcAddr);

#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
    m_debug_messenger = m_instance.createDebugUtilsMessengerEXT(debug_messenger_create_info(), nullptr, m_dispatch_loader);
#endif

    // create window surface
    VkSurfaceKHR surface;
    if (SDL_Vulkan_CreateSurface(window.native_handle(), m_instance, &surface) != SDL_TRUE) {
        throw std::runtime_error(std::string("failed to create window surface: ") + SDL_GetError());
    }

    m_surface = vk::SurfaceKHR(surface);

    // select GPU from available hardware options
    auto gpus = m_instance.enumeratePhysicalDevices();
    std::optional<vk::PhysicalDevice> discrete_gpu;
    std::optional<vk::PhysicalDevice> integrated_gpu;

    // search available GPUs for a suitable one
    for (const auto& gpu : gpus) {
        auto swapchain_suitable = false;
        auto props = gpu.getProperties();
        auto indices = queue_families(gpu, m_surface);
        bool required_extensions_supported = extensions_supported(gpu, device_extensions);

        // if required extensions are supported (including SwapchainKHR), we can query swapchain support.
        if (required_extensions_supported) {
            auto swap_support = swapchain_support(gpu, m_surface);
            swapchain_suitable = !swap_support.formats.empty() && !swap_support.present_modes.empty();
        }

        // if we've found a device that meets our requirements, we can track it
        if (indices.is_complete() && required_extensions_supported && swapchain_suitable) {
            if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
                discrete_gpu = gpu;
                break; // if we've found a discrete GPU, we don't need to continue searching
            } else if (props.deviceType == vk::PhysicalDeviceType::eIntegratedGpu) {
                integrated_gpu = gpu;
            }
        }
    }

    // prioritize discrete GPUs over integrated GPUs
    if (discrete_gpu.has_value()) {
        m_physical_device = discrete_gpu.value();
    } else if (integrated_gpu.has_value()) {
        m_physical_device = integrated_gpu.value();
    } else {
        throw std::runtime_error("No suitable GPU device found.");
    }

    // log the selected GPU
    auto properties = m_physical_device.getProperties();
    Log::trace("Selected GPU: {} (device id: {})", std::string_view(properties.deviceName), properties.deviceID);

    // create the logical gpu
    auto indices = queue_families(m_physical_device, m_surface);
    std::vector<vk::DeviceQueueCreateInfo> queue_create_infos;
    std::set<u32> unique_queue_families = { indices.graphics.value(), indices.present.value() };

    // configure GPU queues
    constexpr float queue_priority[] = { 1.0 };
    queue_create_infos.reserve(unique_queue_families.size());
    for (u32 queue_family : unique_queue_families) {
        queue_create_infos.push_back(vk::DeviceQueueCreateInfo({}, queue_family, queue_priority));
    }

    // configure and create logical device
    vk::DeviceCreateInfo create_info({}, queue_create_infos, validation_layers, device_extensions);
    m_device = m_physical_device.createDevice(create_info);

    // get device queue handles
    m_graphics_queue = m_device.getQueue(indices.graphics.value(), 0);
    m_present_queue = m_device.getQueue(indices.present.value(), 0);

    // create command buffers
    m_cmd_ring = std::make_unique<CommandBufferRing>();
    m_cmd_ring->init(this, m_device, indices.graphics.value());

    // create swapchain
    create_swapchain();

    // create default render pass
    vk::AttachmentDescription color_attachment(
            {},
            m_swapchain_format,
            vk::SampleCountFlagBits::e1,
            vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore,
            vk::AttachmentLoadOp::eDontCare,
            vk::AttachmentStoreOp::eDontCare,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::ePresentSrcKHR
    );

    vk::AttachmentReference color_attachment_ref(0, vk::ImageLayout::eColorAttachmentOptimal);
    vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, {}, color_attachment_ref);
    vk::SubpassDependency dependency(
            VK_SUBPASS_EXTERNAL,
            0,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            {},
            vk::AccessFlagBits::eColorAttachmentWrite,
            {}
    );

    // create render pass
    vk::RenderPassCreateInfo render_pass_info({}, color_attachment, subpass, dependency);
    m_render_pass = m_device.createRenderPass(render_pass_info);

    // create swapchain framebuffers
    create_framebuffers();

    // create sync objects
    m_in_flight_fences.resize(k_max_frames_in_flight);
    m_render_finished_semaphores.resize(k_max_frames_in_flight);
    m_image_available_semaphores.resize(k_max_frames_in_flight);
    for (u32 i = 0; i < k_max_frames_in_flight; i++) {
        m_in_flight_fences[i] = m_device.createFence(vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
        m_image_available_semaphores[i] = m_device.createSemaphore({});
        m_render_finished_semaphores[i] = m_device.createSemaphore({});
    }

    // create VMA allocator
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = m_physical_device;
    allocator_info.device = m_device;
    allocator_info.instance = m_instance;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&allocator_info, &m_vma));

    m_initialized = true;
}

RenderDevice::~RenderDevice() {
    if (m_initialized) destroy();
}

void RenderDevice::create_swapchain() {
    // query the swapchain features and properties the physical device supports
    auto swap_info = swapchain_support(m_physical_device, m_surface);
    auto& present_modes = swap_info.present_modes;

    // select swapchain format; see if RGBA SRGB colors are available, otherwise default to first available format
    vk::SurfaceFormatKHR swap_format = swap_info.formats[0];
    for (const auto& format : swap_info.formats) {
        if (format.format == vk::Format::eB8G8R8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            swap_format = format;
            break;
        }
    }

    // select the swapchain present mode. Ideally we want VK_PRESENT_MODE_MAILBOX if it's available
    vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo; // eFifo is guaranteed to be available, so it's the default.
    if (std::ranges::find(present_modes, vk::PresentModeKHR::eMailbox) != present_modes.end()) {
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    // now we need to determine the swapchain image size
    vk::Extent2D extent = swap_info.capabilities.currentExtent;
    if (swap_info.capabilities.currentExtent.width == UINT32_MAX) {
        // determine the min and max swapchain image sizes
        const auto& min_extent = swap_info.capabilities.minImageExtent;
        const auto& max_extent = swap_info.capabilities.maxImageExtent;

        // get current window size
        u32 width = m_window.width();
        u32 height = m_window.height();

        // set the extent to the closest size the surface will allow on this device
        extent.width = std::clamp(width, min_extent.width, max_extent.width);
        extent.height = std::clamp(height, min_extent.height, max_extent.height);
    }

    // determine the number of swapchain images we'll use
    u32 image_count = swap_info.capabilities.minImageCount + 1;
    if (swap_info.capabilities.maxImageCount > 0 && image_count > swap_info.capabilities.maxImageCount) {
        image_count = swap_info.capabilities.maxImageCount;
    }

    // configure the swapchain for creation
    auto indices = queue_families(m_physical_device, m_surface);
    u32 queue_indices[] = { indices.graphics.value(), indices.present.value() };
    vk::SwapchainCreateInfoKHR create_info(
            {},
            m_surface,
            image_count,
            swap_format.format,
            swap_format.colorSpace,
            extent,
            1,
            vk::ImageUsageFlagBits::eColorAttachment,
            indices.graphics.value() == indices.present.value() ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent,
            queue_indices,
            swap_info.capabilities.currentTransform,
            vk::CompositeAlphaFlagBitsKHR::eOpaque,
            present_mode,
            true
    );

    // create swapchain and cache swapchain state
    m_swapchain = m_device.createSwapchainKHR(create_info);
    m_swapchain_extent = extent;
    m_swapchain_format = swap_format.format;
    m_swapchain_images = m_device.getSwapchainImagesKHR(m_swapchain);

    // create the swapchain image views
    m_swapchain_image_views.resize(m_swapchain_images.size());
    for (int i = 0; i < m_swapchain_images.size(); ++i) {
        vk::ImageViewCreateInfo image_view_create_info(
                {},
                m_swapchain_images[i],
                vk::ImageViewType::e2D,
                m_swapchain_format,
                vk::ComponentMapping(),
                vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)
        );

        m_swapchain_image_views[i] = m_device.createImageView(image_view_create_info);
    }

    Log::trace("Swapchain created({} x {})", m_swapchain_extent.width, m_swapchain_extent.height);
}

void RenderDevice::destroy_swapchain() {
    // destroy framebuffers
    for (auto framebuffer : m_swapchain_framebuffers) {
        m_device.destroyFramebuffer(framebuffer);
    }

    // destroy image views
    for (auto image_view : m_swapchain_image_views) {
        m_device.destroyImageView(image_view);
    }

    // destroy swapchain
    m_device.destroySwapchainKHR(m_swapchain);
}

void RenderDevice::recreate_swapchain() {
    // if the window is minimized, wait until it isn't
    auto [width, height] = m_window.size();
    while (width == 0 || height == 0) {
        width = m_window.width();
        height = m_window.height();
        SDL_WaitEvent(nullptr);
    }

    // recreate swapchain
    m_device.waitIdle();
    destroy_swapchain();
    create_swapchain();
    create_framebuffers();

    m_resized = false;
}

void RenderDevice::create_framebuffers() {
    m_swapchain_framebuffers.resize(m_swapchain_images.size());
    for (int i = 0; i < m_swapchain_framebuffers.size(); ++i) {
        vk::ImageView attachments[] = { m_swapchain_image_views[i] };

        vk::FramebufferCreateInfo framebuffer_info({},
                                                   m_render_pass,
                                                   attachments,
                                                   m_swapchain_extent.width,
                                                   m_swapchain_extent.height,
                                                   1);

        m_swapchain_framebuffers[i] = m_device.createFramebuffer(framebuffer_info);
    }
}

u32 RenderDevice::new_frame() {
    // wait until previous frame has finished being rendered to the screen, then reset the fence
    auto _ = m_device.waitForFences(m_in_flight_fences[m_frame], true, UINT64_MAX);

    // acquire a frame
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_image_available_semaphores[m_frame], nullptr,
                                            &m_swapchain_image);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(); // rebuild swapchain as image size does not match surface size
        return m_frame;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("fialed to acquire swapchain image");
    }

    // reset the in flight fence, this needs to be after swapchain resizing to avoid a deadlock
    m_device.resetFences(m_in_flight_fences[m_frame]);

    // reset command pool
    m_cmd_ring->reset_pools(m_frame);

    return m_frame;
}

void RenderDevice::present() {
    std::vector<vk::CommandBuffer> queued_buffers(m_pending_cmds.size());
    for (u32 i = 0; i < m_pending_cmds.size(); i++) {
        queued_buffers[i] = m_pending_cmds[i]->m_cmd;
    }

    // configure wait stages
    vk::PipelineStageFlags wait_stages[] = { vk::PipelineStageFlagBits::eColorAttachmentOutput };
    vk::SubmitInfo submit_info(m_image_available_semaphores[m_frame], wait_stages, queued_buffers,
                               m_render_finished_semaphores[m_frame]);

    // submit RenderContext to graphics queue
    m_graphics_queue.submit(submit_info, m_in_flight_fences[m_frame]);

    // submit fame to present queue
    vk::PresentInfoKHR present_info(m_render_finished_semaphores[m_frame], m_swapchain, m_swapchain_image);
    VkResult result = vkQueuePresentKHR(m_present_queue, &present_info.operator VkPresentInfoKHR &());

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || m_resized) {
        recreate_swapchain();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to present swapchain image");
    }

    // clear queued commands
    m_pending_cmds.clear();

    // advance frame counter
    m_frame = (m_frame + 1) % k_max_frames_in_flight;
}

void RenderDevice::wait_idle() {
    m_device.waitIdle();
}

void RenderDevice::destroy() {
    wait_idle(); // wait until GPU is idle before be start destroying resources

    m_cmd_ring->destroy();
    vmaDestroyAllocator(m_vma);

    // destroy sync objects
    for (int i = 0; i < k_max_frames_in_flight; i++) {
        m_device.destroySemaphore(m_image_available_semaphores[i]);
        m_device.destroySemaphore(m_render_finished_semaphores[i]);
        m_device.destroyFence(m_in_flight_fences[i]);
    }

    m_device.destroyRenderPass(m_render_pass);
    destroy_swapchain();
    m_device.destroy();
    m_instance.destroySurfaceKHR(m_surface);
#if defined(EMBER_DEBUG) || defined(EMBER_PROFILE)
    m_instance.destroyDebugUtilsMessengerEXT(m_debug_messenger, nullptr, m_dispatch_loader);
#endif
    m_instance.destroy();

    m_initialized = false;
}
