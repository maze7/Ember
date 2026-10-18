// Swapchains: a surface for a window, the images the display lends a frame, and the presents at
// the end of each frame. Backbuffers are texture pool entries the swapchain owns, so a render pass
// draws into one like any other target. A swapchain follows the window: an acquire that finds the
// window resized, or the driver saying so, builds a new chain and retires the old through the
// graveyard, where the frames that could still touch it keep it alive.

#include <ember/gpu/gpu.h>

#include <ember/core/jobs.h>
#include <ember/core/profile.h>
#include <gpu/vulkan/backend.h>

#if defined(EMBER_HAS_PLATFORM)
	#include <ember/platform/platform.h>
	#include <platform/wsi.h>
#endif

namespace ember::gpu
{
	namespace
	{
		// Eight bit sRGB, BGRA first since it is the native order of every desktop compositor,
		// then the UNORM fallbacks. HDR is a later extension.
		VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice adapter, VkSurfaceKHR surface)
		{
			constexpr u32 MAX_FORMATS = 64;
			VkSurfaceFormatKHR formats[MAX_FORMATS];
			u32 count = 0;

			vkGetPhysicalDeviceSurfaceFormatsKHR(adapter, surface, &count, nullptr);
			count = count < MAX_FORMATS ? count : MAX_FORMATS;
			vkGetPhysicalDeviceSurfaceFormatsKHR(adapter, surface, &count, formats);

			constexpr VkFormat PREFERRED[] = {
			    VK_FORMAT_B8G8R8A8_SRGB,
			    VK_FORMAT_R8G8B8A8_SRGB,
			    VK_FORMAT_B8G8R8A8_UNORM,
			    VK_FORMAT_R8G8B8A8_UNORM,
			};

			for (const VkFormat wanted : PREFERRED)
			{
				for (u32 i = 0; i < count; ++i)
				{
					if (formats[i].format == wanted &&
					    formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
					{
						return formats[i];
					}
				}
			}

			EMBER_WARN("vulkan: no preferred swapchain format; using the surface's first");
			return count > 0 ? formats[0]
			                 : VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB,
			                       VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
		}

		// The requested mode when the surface offers it, otherwise FIFO, the one every surface has.
		VkPresentModeKHR choose_present_mode(VkPhysicalDevice adapter, VkSurfaceKHR surface,
		    PresentMode requested)
		{
			if (requested == PresentMode::VSync)
			{
				return VK_PRESENT_MODE_FIFO_KHR;
			}

			const VkPresentModeKHR wanted = requested == PresentMode::Mailbox
			                                    ? VK_PRESENT_MODE_MAILBOX_KHR
			                                    : VK_PRESENT_MODE_IMMEDIATE_KHR;

			constexpr u32 MAX_MODES = 8;
			VkPresentModeKHR modes[MAX_MODES];
			u32 count = 0;

			vkGetPhysicalDeviceSurfacePresentModesKHR(adapter, surface, &count, nullptr);
			count = count < MAX_MODES ? count : MAX_MODES;
			vkGetPhysicalDeviceSurfacePresentModesKHR(adapter, surface, &count, modes);

			for (u32 i = 0; i < count; ++i)
			{
				if (modes[i] == wanted)
				{
					return wanted;
				}
			}

			EMBER_INFO("vulkan: the requested present mode is unavailable; using VSync");
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		// A Wayland compositor may offer only premultiplied, so opaque is a preference, not a rule.
		VkCompositeAlphaFlagBitsKHR choose_composite_alpha(const VkSurfaceCapabilitiesKHR& caps)
		{
			constexpr VkCompositeAlphaFlagBitsKHR PREFERRED[] = {
			    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
			    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
			    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
			};

			for (const VkCompositeAlphaFlagBitsKHR alpha : PREFERRED)
			{
				if ((caps.supportedCompositeAlpha & alpha) != 0)
				{
					return alpha;
				}
			}

			return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		}

		u32 clamp(u32 value, u32 low, u32 high)
		{
			return value < low ? low : (value > high ? high : value);
		}

		// The window's size in pixels. The surface says when it can; when it cannot, which is
		// Wayland, the window does, and only main's fiber may ask it. Zero is minimized.
		VkExtent2D resolve_extent(const vk::SwapchainData& data,
		    const VkSurfaceCapabilitiesKHR& caps)
		{
			if (caps.currentExtent.width != ~0u)
			{
				return caps.currentExtent;
			}

			Extent2D pixels = {data.extent.width, data.extent.height};
#if defined(EMBER_HAS_PLATFORM)
			if (jobs::is_main())
			{
				pixels = platform::window_size_in_pixels(data.window);
			}
#endif

			return {
			    clamp(pixels.width, caps.minImageExtent.width, caps.maxImageExtent.width),
			    clamp(pixels.height, caps.minImageExtent.height, caps.maxImageExtent.height),
			};
		}

		// Retires the chain's images: the pool handles die now, the views and present semaphores
		// go when the frames that could touch them have completed.
		void retire_images(vk::Backend& b, vk::SwapchainData& data)
		{
			for (u32 i = 0; i < data.image_count; ++i)
			{
				vk::release_backbuffer(b, data.images[i]);
				data.images[i] = {};

				b.resources_lock.lock();
				vk::bury(b,
				    {.kind = vk::Dead::Kind::Semaphore, .handle = (u64)data.present_semaphores[i]});
				b.resources_lock.unlock();
				data.present_semaphores[i] = VK_NULL_HANDLE;
			}

			data.image_count = 0;
		}

		enum class Build : u8
		{
			Ok,

			// Zero extent: minimized. Nothing was built and any existing chain is kept.
			Suspended,
			Failed,
		};

		// Makes the chain and its backbuffers, handing the driver the old chain to recycle. The
		// old generation retires through the graveyard.
		Build build(vk::Backend& b, vk::SwapchainData& data, SwapchainHandle handle)
		{
			EMBER_PROFILE_SCOPE_C("gpu: build swapchain", PROFILE_COLOR_RENDER);

			VkSurfaceCapabilitiesKHR caps = {};
			if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(b.ctx.adapter, data.surface, &caps) !=
			    VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: the surface capability query failed");
				return Build::Failed;
			}

			const VkExtent2D extent = resolve_extent(data, caps);
			if (extent.width == 0 || extent.height == 0)
			{
				return Build::Suspended;
			}

			// A max of zero means unbounded; the engine's own cap keeps the per image arrays
			// indexable.
			u32 image_count = data.preferred_image_count > caps.minImageCount
			                      ? data.preferred_image_count
			                      : caps.minImageCount;
			image_count = clamp(image_count, 1, vk::MAX_SWAPCHAIN_IMAGES);
			if (caps.maxImageCount != 0 && image_count > caps.maxImageCount)
			{
				image_count = caps.maxImageCount;
			}

			data.surface_format = choose_surface_format(b.ctx.adapter, data.surface);
			data.present_mode =
			    choose_present_mode(b.ctx.adapter, data.surface, data.requested_mode);

			// Copies in and out when offered: screenshots, and a blit from an offscreen target.
			VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			usage |= caps.supportedUsageFlags &
			         (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

			const VkSwapchainKHR old = data.swapchain;
			const VkSwapchainCreateInfoKHR info{
			    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			    .surface = data.surface,
			    .minImageCount = image_count,
			    .imageFormat = data.surface_format.format,
			    .imageColorSpace = data.surface_format.colorSpace,
			    .imageExtent = extent,
			    .imageArrayLayers = 1,
			    .imageUsage = usage,
			    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			    .preTransform = caps.currentTransform,
			    .compositeAlpha = choose_composite_alpha(caps),
			    .presentMode = data.present_mode,
			    .clipped = VK_TRUE,
			    .oldSwapchain = old,
			};

			VkSwapchainKHR swapchain = VK_NULL_HANDLE;
			if (const VkResult result =
			        vkCreateSwapchainKHR(b.ctx.device, &info, &b.ctx.callbacks, &swapchain);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: vkCreateSwapchainKHR failed: {}", vk::result_name(result));
				return Build::Failed;
			}

			retire_images(b, data);
			if (old != VK_NULL_HANDLE)
			{
				b.resources_lock.lock();
				vk::bury(b, {.kind = vk::Dead::Kind::Swapchain, .handle = (u64)old});
				b.resources_lock.unlock();
			}

			data.swapchain = swapchain;
			data.extent = extent;

			VkImage images[vk::MAX_SWAPCHAIN_IMAGES];
			u32 count = 0;
			EMBER_VK_CHECK(vkGetSwapchainImagesKHR(b.ctx.device, swapchain, &count, nullptr));

			// The per image tables index by the driver's image index, so a chain with more images
			// than they hold is unusable. Never acquired from, so it may go at once.
			if (count > vk::MAX_SWAPCHAIN_IMAGES)
			{
				EMBER_ERROR("vulkan: the swapchain has {} images; the engine supports {}", count,
				    vk::MAX_SWAPCHAIN_IMAGES);
				vkDestroySwapchainKHR(b.ctx.device, swapchain, &b.ctx.callbacks);
				data.swapchain = VK_NULL_HANDLE;
				return Build::Failed;
			}

			EMBER_VK_CHECK(vkGetSwapchainImagesKHR(b.ctx.device, swapchain, &count, images));

			const TextureFormat format = vk::from_vk(data.surface_format.format);
			const VkSemaphoreCreateInfo semaphore_info{
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

			for (u32 i = 0; i < count; ++i)
			{
				const VkImageViewCreateInfo view_info{
				    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				    .image = images[i],
				    .viewType = VK_IMAGE_VIEW_TYPE_2D,
				    .format = data.surface_format.format,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
				};

				VkImageView view = VK_NULL_HANDLE;
				EMBER_VK_CHECK(
				    vkCreateImageView(b.ctx.device, &view_info, &b.ctx.callbacks, &view));
				EMBER_VK_CHECK(vkCreateSemaphore(b.ctx.device, &semaphore_info, &b.ctx.callbacks,
				    &data.present_semaphores[i]));

				data.images[i] = vk::wrap_backbuffer(b, images[i], view, extent, format, handle, i);
				data.image_count = i + 1;

				if (data.images[i].is_null())
				{
					EMBER_ERROR("vulkan: the texture pool is full; no slot for a backbuffer");
					vkDestroyImageView(b.ctx.device, view, &b.ctx.callbacks);
					data.image_count = i;
					retire_images(b, data);
					vkDestroySwapchainKHR(b.ctx.device, swapchain, &b.ctx.callbacks);
					data.swapchain = VK_NULL_HANDLE;
					return Build::Failed;
				}

				vk::set_name(b.ctx, VK_OBJECT_TYPE_IMAGE, (u64)images[i], "ember.backbuffer");
			}

			data.acquired_frame = 0;
			data.needs_recreate = false;
			return Build::Ok;
		}

		Backbuffer backbuffer_of(const vk::SwapchainData& data)
		{
			return {
			    .texture = data.images[data.acquired_image],
			    .extent = {data.extent.width, data.extent.height},
			    .format = vk::from_vk(data.surface_format.format),
			};
		}

		struct AcquireWait
		{
			VkDevice device = VK_NULL_HANDLE;
			VkSwapchainKHR swapchain = VK_NULL_HANDLE;
			VkSemaphore semaphore = VK_NULL_HANDLE;
			u32 image = 0;
			VkResult result = VK_SUCCESS;
		};

		void acquire_job(void* data)
		{
			AcquireWait& wait = *(AcquireWait*)data;
			wait.result = vkAcquireNextImageKHR(wait.device, wait.swapchain, UINT64_MAX,
			    wait.semaphore, VK_NULL_HANDLE, &wait.image);
		}

		void image_barrier(VkCommandBuffer commands, VkImage image, TextureState before,
		    TextureState after, VkPipelineStageFlags2 shader_stages)
		{
			const vk::StateInfo src = vk::state_info(before, shader_stages);
			const vk::StateInfo dst = vk::state_info(after, shader_stages);

			const VkImageMemoryBarrier2 barrier{
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			    .srcStageMask = src.stages,
			    .srcAccessMask = src.access,
			    .dstStageMask = dst.stages,
			    .dstAccessMask = dst.access,
			    .oldLayout = src.layout,
			    .newLayout = dst.layout,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image = image,
			    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};

			const VkDependencyInfo dependency{
			    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			    .imageMemoryBarrierCount = 1,
			    .pImageMemoryBarriers = &barrier,
			};
			vkCmdPipelineBarrier2(commands, &dependency);
		}
	}

	namespace vk
	{
		void record_finish(Backend& b, Slot& slot, VkCommandBuffer commands)
		{
			for (u32 i = 0; i < slot.present_count; ++i)
			{
				const PendingPresent& pending = slot.presents[i];
				SwapchainData* data = b.swapchains.get(pending.swapchain);
				if (data == nullptr)
				{
					continue;
				}

				const TextureHot* hot = b.textures.get(data->images[pending.image]);
				if (hot == nullptr || data->acquired_state == TextureState::Present)
				{
					continue;
				}

				TextureState state = data->acquired_state;

				// A frame that drew nothing presents a dark clear rather than garbage.
				if (state == TextureState::Undefined)
				{
					image_barrier(commands, hot->image, TextureState::Undefined,
					    TextureState::ColorTarget, b.ctx.all_shader_stages);

					const VkRenderingAttachmentInfo color{
					    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
					    .imageView = hot->view,
					    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
					    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
					    .clearValue = {.color = {.float32 = {0.02f, 0.025f, 0.035f, 1.0f}}},
					};

					const VkRenderingInfo rendering{
					    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
					    .renderArea = {{0, 0}, data->extent},
					    .layerCount = 1,
					    .colorAttachmentCount = 1,
					    .pColorAttachments = &color,
					};

					vkCmdBeginRendering(commands, &rendering);
					vkCmdEndRendering(commands);
					state = TextureState::ColorTarget;
				}

				image_barrier(commands, hot->image, state, TextureState::Present,
				    b.ctx.all_shader_stages);
				data->acquired_state = TextureState::Present;
			}
		}

		u32 present_signals(Backend& b, Slot& slot, VkSemaphoreSubmitInfo* signals)
		{
			u32 count = 0;
			for (u32 i = 0; i < slot.present_count; ++i)
			{
				const PendingPresent& pending = slot.presents[i];
				if (const SwapchainData* data = b.swapchains.get(pending.swapchain))
				{
					// All commands: the present transition retires in no stage, so a narrower
					// scope would not cover it.
					signals[count++] = {
					    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
					    .semaphore = data->present_semaphores[pending.image],
					    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					};
				}
			}

			return count;
		}

		void present(Backend& b, Slot& slot)
		{
			if (slot.present_count == 0)
			{
				return;
			}

			EMBER_PROFILE_SCOPE_C("gpu: present", PROFILE_COLOR_RENDER);

			VkSwapchainKHR swapchains[MAX_SWAPCHAINS];
			VkSemaphore waits[MAX_SWAPCHAINS];
			u32 images[MAX_SWAPCHAINS];
			VkResult results[MAX_SWAPCHAINS];
			SwapchainData* datas[MAX_SWAPCHAINS];
			u32 count = 0;

			for (u32 i = 0; i < slot.present_count; ++i)
			{
				const PendingPresent& pending = slot.presents[i];
				SwapchainData* data = b.swapchains.get(pending.swapchain);
				if (data == nullptr)
				{
					continue;
				}

				datas[count] = data;
				swapchains[count] = data->swapchain;
				waits[count] = data->present_semaphores[pending.image];
				images[count] = pending.image;
				++count;
			}

			slot.present_count = 0;
			if (count == 0)
			{
				return;
			}

			const VkPresentInfoKHR info{
			    .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			    .waitSemaphoreCount = count,
			    .pWaitSemaphores = waits,
			    .swapchainCount = count,
			    .pSwapchains = swapchains,
			    .pImageIndices = images,
			    .pResults = results,
			};

			QueueState& queue = queue_for(b, Queue::Graphics);
			queue.lock.lock();
			const VkResult result = vkQueuePresentKHR(queue.handle, &info);
			queue.lock.unlock();
			note_result(b, result);

			// Out of date or suboptimal here means a rebuild at the next acquire.
			for (u32 i = 0; i < count; ++i)
			{
				if (results[i] == VK_ERROR_OUT_OF_DATE_KHR || results[i] == VK_SUBOPTIMAL_KHR)
				{
					datas[i]->needs_recreate = true;
				}
			}
		}
	}

	SwapchainHandle create_swapchain(const SwapchainDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		EMBER_ASSERT(jobs::is_main());

#if !defined(EMBER_HAS_PLATFORM)
		(void)def;
		(void)build;
		EMBER_ERROR("gpu: this build has no window system; no swapchain can be made");
		return {};
#else
		if (b->ctx.headless)
		{
			EMBER_ERROR("gpu: a headless device cannot make a swapchain");
			return {};
		}

		b->resources_lock.lock();
		const SwapchainHandle handle = b->swapchains.emplace();
		b->resources_lock.unlock();

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: the swapchain pool is full");
			return {};
		}

		vk::SwapchainData& data = *b->swapchains.get(handle);
		data.window = def.window;
		data.requested_mode = def.present_mode;
		data.preferred_image_count = def.image_count;

		// The surface outlives every generation of the chain for this window.
		data.surface = platform::vulkan::create_surface(def.window, b->ctx.instance);
		if (data.surface == VK_NULL_HANDLE)
		{
			EMBER_ERROR("gpu: the window cannot carry a Vulkan surface");
			b->resources_lock.lock();
			(void)b->swapchains.erase(handle);
			b->resources_lock.unlock();
			return {};
		}

		// Suspended is fine: a window made minimized builds at its first acquire with pixels.
		if (build(*b, data, handle) == Build::Failed)
		{
			destroy_swapchain(handle);
			return {};
		}

		const VkSemaphoreCreateInfo semaphore_info{
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		for (u32 i = 0; i < b->ctx.in_flight; ++i)
		{
			EMBER_VK_CHECK(vkCreateSemaphore(b->ctx.device, &semaphore_info, &b->ctx.callbacks,
			    &data.acquire_semaphores[i]));
		}

		return handle;
#endif
	}

	void destroy_swapchain(SwapchainHandle swapchain)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		EMBER_ASSERT(jobs::is_main());

		vk::SwapchainData* data = b->swapchains.get(swapchain);
		if (data == nullptr)
		{
			return;
		}

		const vk::Slot& acquirer = b->slots[data->acquired_frame % b->ctx.in_flight];
		EMBER_ASSERT_MSG(data->acquired_frame == 0 ||
		                     acquirer.frame_index != data->acquired_frame || acquirer.closed,
		    "a swapchain destroyed in the frame that acquired from it");
		(void)acquirer;

		retire_images(*b, *data);

		b->resources_lock.lock();
		for (u32 i = 0; i < b->ctx.in_flight; ++i)
		{
			vk::bury(*b,
			    {.kind = vk::Dead::Kind::Semaphore, .handle = (u64)data->acquire_semaphores[i]});
		}
		vk::bury(*b, {.kind = vk::Dead::Kind::Swapchain, .handle = (u64)data->swapchain});
		vk::bury(*b, {.kind = vk::Dead::Kind::Surface, .handle = (u64)data->surface});
		(void)b->swapchains.erase(swapchain);
		b->resources_lock.unlock();
	}

	bool is_valid(SwapchainHandle swapchain)
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->swapchains.get(swapchain) != nullptr;
	}

	TextureFormat swapchain_format(SwapchainHandle swapchain)
	{
		const vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return TextureFormat::Undefined;
		}

		const vk::SwapchainData* data = b->swapchains.get(swapchain);
		return data != nullptr ? vk::from_vk(data->surface_format.format)
		                       : TextureFormat::Undefined;
	}

	Backbuffer acquire(const Frame& frame, SwapchainHandle swapchain)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		vk::SwapchainData* data = b->swapchains.get(swapchain);
		if (data == nullptr)
		{
			EMBER_ERROR("gpu: acquire on a stale swapchain");
			return {};
		}

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(!slot.closed, "an acquire for a frame that has ended");

		EMBER_PROFILE_SCOPE_C("gpu: acquire", PROFILE_COLOR_WAIT);

		// Held across the park, which a JobMutex may be: a second job asking for the same image
		// waits here and then takes the cached answer.
		data->lock.lock();

		Backbuffer result = {};
		if (data->acquired_frame == frame.index)
		{
			result = backbuffer_of(*data);
			data->lock.unlock();
			return result;
		}

		bool rebuild = data->needs_recreate || data->swapchain == VK_NULL_HANDLE;

#if defined(EMBER_HAS_PLATFORM)
		// On main's fiber the window's size is at hand; elsewhere the driver says when it changed.
		if (jobs::is_main())
		{
			const Extent2D pixels = platform::window_size_in_pixels(data->window);
			if (pixels.width == 0 || pixels.height == 0)
			{
				data->lock.unlock();
				return result;
			}

			rebuild |= pixels.width != data->extent.width || pixels.height != data->extent.height;
		}
#endif

		if (rebuild && build(*b, *data, swapchain) != Build::Ok)
		{
			data->lock.unlock();
			return result;
		}

		const u32 slot_index = (u32)(frame.index % b->ctx.in_flight);

		// Out of date signals nothing and leaves the semaphore alone, so a retry with the same
		// semaphore is safe. One rebuild, then the frame goes without.
		for (u32 attempt = 0; attempt < 2; ++attempt)
		{
			AcquireWait wait{
			    .device = b->ctx.device,
			    .swapchain = data->swapchain,
			    .semaphore = data->acquire_semaphores[slot_index],
			};

			wait.result = vkAcquireNextImageKHR(wait.device, wait.swapchain, 0, wait.semaphore,
			    VK_NULL_HANDLE, &wait.image);

			if (wait.result == VK_NOT_READY || wait.result == VK_TIMEOUT)
			{
				vk::run_blocking(acquire_job, &wait, "gpu acquire");
			}

			if (wait.result == VK_SUCCESS || wait.result == VK_SUBOPTIMAL_KHR)
			{
				// Suboptimal still signalled: draw and present this one, rebuild at the next.
				data->needs_recreate |= wait.result == VK_SUBOPTIMAL_KHR;
				data->acquired_image = wait.image;
				data->acquired_frame = frame.index;
				data->acquired_state = TextureState::Undefined;

				slot.lock.lock();
				EMBER_ASSERT(slot.present_count < vk::MAX_SWAPCHAINS);
				slot.presents[slot.present_count++] = {.swapchain = swapchain, .image = wait.image};
				slot.acquire_waits[slot.acquire_wait_count++] = wait.semaphore;
				slot.lock.unlock();

				result = backbuffer_of(*data);
				break;
			}

			if (wait.result == VK_ERROR_OUT_OF_DATE_KHR)
			{
				if (build(*b, *data, swapchain) != Build::Ok)
				{
					break;
				}
				continue;
			}

			vk::note_result(*b, wait.result);
			EMBER_ERROR("vulkan: vkAcquireNextImageKHR failed: {}", vk::result_name(wait.result));
			break;
		}

		data->lock.unlock();
		return result;
	}
}
