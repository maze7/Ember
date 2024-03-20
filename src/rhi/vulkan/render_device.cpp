#include "core/platform.h"
#include "core/log.h"
#include "fmt/format.h"
#include "system/window.h"
#include "rhi/vulkan/render_device.h"
#include "rhi/vulkan/command_buffer.h"
#include "rhi/vulkan/command_buffer_ring.h"
#include "rhi/vulkan/shader.h"
#include "rhi/vulkan/buffer.h"
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

    // create descriptor pools
    static constexpr u32 k_global_pool_elements = 128;
    static constexpr u32 k_descriptor_sets_pool_size = 4096;

    vk::DescriptorPoolSize pool_sizes[] = {
        //{ vk::DescriptorType::eSampler, k_global_pool_elements },
        // { vk::DescriptorType::eCombinedImageSampler, k_global_pool_elements },
        // { vk::DescriptorType::eSampledImage, k_global_pool_elements },
        // { vk::DescriptorType::eStorageImage, k_global_pool_elements },
        // { vk::DescriptorType::eUniformTexelBuffer, k_global_pool_elements },
        // { vk::DescriptorType::eStorageTexelBuffer, k_global_pool_elements },
        { vk::DescriptorType::eUniformBuffer, k_global_pool_elements },
        // { vk::DescriptorType::eStorageBuffer, k_global_pool_elements },
        // { vk::DescriptorType::eUniformBufferDynamic, k_global_pool_elements },
        // { vk::DescriptorType::eStorageBufferDynamic, k_global_pool_elements },
        // { vk::DescriptorType::eInputAttachment, k_global_pool_elements },
    };

    vk::DescriptorPoolCreateInfo pool_info({}, k_descriptor_sets_pool_size, pool_sizes);
    m_descriptor_pool = m_device.createDescriptorPool(pool_info);

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
    vk::PresentModeKHR present_mode = vk::PresentModeKHR::eImmediate; // eFifo is guaranteed to be available, so it's the default.
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

    // get_command_buffer a frame
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_image_available_semaphores[m_frame], nullptr,
                                            &m_swapchain_image);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(); // rebuild swapchain as image size does not match surface size
        return m_frame;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("fialed to get_command_buffer swapchain image");
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

    m_device.destroyDescriptorPool(m_descriptor_pool);

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

CommandBuffer *RenderDevice::get_command_buffer(QueueType type, bool begin) {
    auto cb = m_cmd_ring->get_command_buffer(m_frame, begin);

    // TODO(Cal): TEMP HACK - Should this exist in the command buffer?
    // The answer is no.
    cb->m_framebuffer = m_swapchain_framebuffers[m_swapchain_image];
    cb->m_pass = m_render_pass;

    return cb;
}

CommandBuffer *RenderDevice::get_command_buffer_instant() {
    auto cb = m_cmd_ring->get_command_buffer_instant(m_frame, true);
    return cb;
}

void RenderDevice::submit(CommandBuffer *cb, bool immediate) {
    auto buf = (CommandBuffer*) cb;

    // automatically end the command buffer if it wasn't ended
    if (buf->m_recording)
        buf->end();

    // if the submission is an immediate submission, we want to flush it to the queue right away
    if (immediate) {
        vk::SubmitInfo submit_info;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &buf->m_cmd;
        m_graphics_queue.submit(submit_info);
        wait_idle(); // wait until command buffer completes before we hand back control to this thread
    } else { // otherwise, queue it for the next call to present()
        m_pending_cmds.push_back(buf);
    }
}

Handle<Shader> RenderDevice::create_shader(const ShaderDef &def) {
    // module create info
    vk::ShaderModuleCreateInfo vertex_module_info({}, def.vertex.size(), (const u32*)def.vertex.data());
    vk::ShaderModuleCreateInfo fragment_module_info({}, def.fragment.size(), (const u32*)def.fragment.data());

    // create shader modules
    auto vertex_module = m_device.createShaderModule(vertex_module_info);
    auto fragment_module = m_device.createShaderModule(fragment_module_info);

    // define shader stages
    vk::PipelineShaderStageCreateInfo vertex_shader_stage_info({}, vk::ShaderStageFlagBits::eVertex, vertex_module, "main");
    vk::PipelineShaderStageCreateInfo fragment_shader_stage_info({}, vk::ShaderStageFlagBits::eFragment, fragment_module, "main");
    vk::PipelineShaderStageCreateInfo shader_stages[] = { vertex_shader_stage_info, fragment_shader_stage_info };

    // dynamic pipeline state
    vk::DynamicState dynamic_states[] = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    vk::PipelineDynamicStateCreateInfo dynamic_state({}, dynamic_states);

    // create vulkan objects for each vertex buffer input binding
    std::vector<vk::VertexInputBindingDescription> input_bindings(def.vertex_bindings.size());
    std::vector<vk::VertexInputAttributeDescription> input_attributes;

    for (u32 binding = 0; binding < def.vertex_bindings.size(); binding++) {
        input_bindings[binding] = vk::VertexInputBindingDescription(binding, def.vertex_bindings[binding].stride);

        for (u32 attr = 0; attr < def.vertex_bindings[binding].attributes.size(); attr++) {
            auto attribute = def.vertex_bindings[binding].attributes[attr];
            input_attributes.emplace_back(attr, binding, to_vk_vertex_format(attribute.format), (u32)attribute.offset);
        }
    }

    // TODO: For now we hard code vertex data to get something up and running, this should be fleshed out.
    vk::PipelineVertexInputStateCreateInfo vertex_input_info({}, input_bindings, input_attributes);
    vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, vk::PrimitiveTopology::eTriangleList, false);

    // viewport and scissor are dynamic state, so we only define the number of viewports & scissors we expect.
    vk::PipelineViewportStateCreateInfo viewport_state({}, 1, nullptr, 1, nullptr);
    vk::PipelineRasterizationStateCreateInfo rasterizer({}, false, false,
                                                        vk::PolygonMode::eFill, vk::CullModeFlagBits::eFront,
                                                        vk::FrontFace::eCounterClockwise, false,
                                                        0, 0, 0, 1);

    // TODO: For now we hard code multisampling as we're targeting 2D Pixel art games, might be worth allowing this to be configured
    vk::PipelineMultisampleStateCreateInfo multisampling({}, vk::SampleCountFlagBits::e1, false);

    // TODO: This should also be configurable. Hard coding for now while.
    vk::PipelineColorBlendAttachmentState color_blend_attachment;
    color_blend_attachment.blendEnable          = true;
    color_blend_attachment.srcColorBlendFactor  = vk::BlendFactor::eSrcAlpha;
    color_blend_attachment.dstColorBlendFactor  = vk::BlendFactor::eOneMinusSrcAlpha;
    color_blend_attachment.colorBlendOp         = vk::BlendOp::eAdd;
    color_blend_attachment.srcColorBlendFactor  = vk::BlendFactor::eOne;
    color_blend_attachment.dstAlphaBlendFactor  = vk::BlendFactor::eZero;
    color_blend_attachment.alphaBlendOp         = vk::BlendOp::eAdd;
    color_blend_attachment.colorWriteMask       = vk::ColorComponentFlagBits::eR |
                                                  vk::ColorComponentFlagBits::eG |
                                                  vk::ColorComponentFlagBits::eB |
                                                  vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo color_blending({}, false, vk::LogicOp::eCopy, color_blend_attachment);

    // get descriptor set layouts and bind to PipelineLayoutCreateInfo
    std::vector<vk::DescriptorSetLayout> layouts(def.bind_groups.size());
    for (u32 i = 0; i < def.bind_groups.size(); i++) {
        auto binding_layout = m_bind_layouts.get(def.bind_groups[i]);
    	layouts[i] = binding_layout[i].layout;
    }

    // configure pipeline layout with all descriptor layouts
    vk::PipelineLayoutCreateInfo layout_info({}, layouts);

    // create pipeline layout
    auto layout = m_device.createPipelineLayout(layout_info);

    // create graphics pipeline
    vk::GraphicsPipelineCreateInfo pipeline_info({},
                                                 shader_stages,
                                                 &vertex_input_info,
                                                 &input_assembly,
                                                 nullptr, // tesselation state
                                                 &viewport_state,
                                                 &rasterizer,
                                                 &multisampling,
                                                 nullptr, // depth-stencil-state
                                                 &color_blending,
                                                 &dynamic_state,
                                                 layout,
                                                 m_render_pass,
                                                 0);

    auto [result, pipeline] = m_device.createGraphicsPipeline({}, pipeline_info);

    // cleanup individual shader modules
    m_device.destroyShaderModule(vertex_module);
    m_device.destroyShaderModule(fragment_module);

    if (result == vk::Result::eSuccess) {
        return m_shaders.emplace(pipeline, layout, m_render_pass);
    } else {
        throw std::runtime_error("error creating pipeline object");
    }
}

Shader *RenderDevice::get_shader(Handle<Shader> handle) {
    return (Shader*) m_shaders.get(handle);
}

void RenderDevice::destroy_shader(Handle<Shader> handle) {
    auto shader = m_shaders.get(handle);

    // destroy underlying vulkan objects
    m_device.destroyPipelineLayout(shader->layout);
    m_device.destroyPipeline(shader->pso);

    // erase the shader from the pool
    m_shaders.erase(handle);
}

Handle<Buffer> RenderDevice::create_buffer(const BufferDef &def) {
    auto handle = m_buffers.emplace();
    auto buf = m_buffers.get(handle);

    buf->size = def.size;
    buf->usage = def.usage;
    buf->memory_type = def.memory;

    VkBufferCreateInfo buf_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buf_info.usage = to_vk_buffer_usage(def.usage);
    buf_info.size = def.size;

    VmaAllocationCreateInfo mem_info{};
    mem_info.usage = to_vk_memory_type(def.memory);
    mem_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_HOST_ACCESS_ALLOW_TRANSFER_INSTEAD_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo alloc_info{};
    VkBuffer temp_buffer{};

    VK_CHECK(vmaCreateBuffer(m_vma, &buf_info, &mem_info, &temp_buffer, &buf->vma_allocation, &alloc_info));
    buf->device_memory = alloc_info.deviceMemory;
    buf->data = (u8*)alloc_info.pMappedData;
    buf->buffer = vk::Buffer(temp_buffer);

    vmaGetAllocationMemoryProperties(m_vma, buf->vma_allocation, &buf->memory_flags);

    if (def.data != nullptr) {
        memcpy(buf->data, def.data, def.size);
    }

    return handle;
}

Buffer* RenderDevice::get_buffer(Handle<Buffer> handle) {
    return m_buffers.get(handle);
}

void RenderDevice::destroy_buffer(Handle<Buffer> handle) {
    auto buf = m_buffers.get(handle);
    vmaDestroyBuffer(m_vma, buf->buffer, buf->vma_allocation);
    m_buffers.erase(handle);
}

Handle<BindLayout> RenderDevice::create_bind_layout(const BindLayoutDef& def) {
    auto handle = m_bind_layouts.emplace();
    auto layout = m_bind_layouts.get(handle);
    
    std::vector<vk::DescriptorSetLayoutBinding> bindings(def.bindings.size());
    
    // configure descriptor set bindings
    u32 index = 0;
    for (u32 i = 0; i < def.bindings.size(); i++) {
        bindings[i] = vk::DescriptorSetLayoutBinding(
            index, 
            to_vk_binding_type(def.bindings[i].type),
            def.bindings[i].count,
            to_vk_shader_stage(def.bindings[i].stage),
            {} // TODO: We should be able to select this from the abstraction layer, this should probably exist in BindingDef struct.
        ); 

        index += def.bindings[i].count;
    }

    // create descriptor set layout
    vk::DescriptorSetLayoutCreateInfo layout_info({}, bindings);
    layout->layout = m_device.createDescriptorSetLayout(layout_info);

    return handle;
}

BindLayout* RenderDevice::get_bind_layout(Handle<BindLayout> handle) {
    return m_bind_layouts.get(handle);
}

void RenderDevice::destroy_bind_layout(Handle<BindLayout> handle) {
    auto layout = m_bind_layouts.get(handle);

    // destroy vulkan layout
    m_device.destroyDescriptorSetLayout(layout->layout);

    m_bind_layouts.erase(handle);
}

Handle<BindGroup> RenderDevice::create_bind_group(const BindGroupDef& def) {
    auto handle = m_bind_groups.emplace();
    auto group = m_bind_groups.get(handle);
    auto layout = m_bind_layouts.get(def.layout);

    // allocate a descriptor set
    vk::DescriptorSetAllocateInfo alloc_info(m_descriptor_pool, layout->layout);
    group->descriptor_set = m_device.allocateDescriptorSets(alloc_info)[0];

    // build descriptor writes for buffers
    std::vector<vk::WriteDescriptorSet> writes(def.buffers.size());
    for (u32 i = 0; i < writes.size(); i++) {
        writes[i] = vk::WriteDescriptorSet(group->descriptor_set, i, 0, 1);

        auto buf = m_buffers.get(def.buffers[i]);
        writes[i].descriptorType = vk::DescriptorType::eUniformBuffer;
        vk::DescriptorBufferInfo buffer_info(buf->buffer, 0, buf->size);
        writes[i].pBufferInfo = &buffer_info;
    }

    m_device.updateDescriptorSets(writes, {});

    return handle;
}

BindGroup* RenderDevice::get_bind_group(Handle<BindGroup> handle) {
    return m_bind_groups.get(handle);
}
