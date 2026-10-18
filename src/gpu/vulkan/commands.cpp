// Recording. Every function resolves its handles lock free, translates to one Vulkan call and
// returns. A list that is not open asserts in a debug build and does nothing otherwise.

#include <ember/gpu/commands.h>

#include <gpu/vulkan/backend.h>

namespace ember::gpu
{
	namespace
	{
		bool recording(const CommandList* list)
		{
			EMBER_ASSERT_MSG(list != nullptr && list->open,
			    "a command on a list that is not recording");
			return list != nullptr && list->open;
		}

		struct BufferStateInfo
		{
			VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 access = VK_ACCESS_2_NONE;
		};

		BufferStateInfo buffer_state_info(BufferState state, VkPipelineStageFlags2 shader_stages)
		{
			switch (state)
			{
				case BufferState::ShaderRead:
					return {shader_stages, VK_ACCESS_2_SHADER_READ_BIT};
				case BufferState::ShaderWrite:
					return {shader_stages,
					    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
				case BufferState::Index:
					return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};
				case BufferState::Indirect:
					return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
					    VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};

				// Fills run at the clear stage and copies at the copy stage; one state covers every
				// transfer command a pass can record.
				case BufferState::CopySrc:
					return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
				case BufferState::CopyDst:
					return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
				case BufferState::Count:
					break;
			}

			EMBER_UNREACHABLE();
		}

		VkAttachmentLoadOp to_vk(LoadOp op)
		{
			switch (op)
			{
				case LoadOp::Load:
					return VK_ATTACHMENT_LOAD_OP_LOAD;
				case LoadOp::Clear:
					return VK_ATTACHMENT_LOAD_OP_CLEAR;
				case LoadOp::DontCare:
				case LoadOp::Count:
					break;
			}

			return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		}

		VkAttachmentStoreOp to_vk(StoreOp op)
		{
			return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE
			                            : VK_ATTACHMENT_STORE_OP_DONT_CARE;
		}

		// A texture with one subresource attaches through its whole view; one with more goes
		// through the slice its creation built for the mip and layer.
		VkImageView attachment_view(const vk::TextureHot& hot, const vk::TextureCold& cold, u32 mip,
		    u32 layer)
		{
			if (cold.attachment_views == nullptr)
			{
				EMBER_ASSERT(mip == 0 && layer == 0);
				return hot.view;
			}

			EMBER_ASSERT(mip < cold.mip_count && layer < cold.layer_count);
			return cold.attachment_views[mip * cold.layer_count + layer];
		}

		// The public contract is a top left origin with positive height on every backend. The
		// negative height here is what makes clip space Y up over Vulkan.
		VkViewport flip(const Viewport& viewport)
		{
			return {
			    .x = viewport.x,
			    .y = viewport.y + viewport.height,
			    .width = viewport.width,
			    .height = -viewport.height,
			    .minDepth = viewport.min_depth,
			    .maxDepth = viewport.max_depth,
			};
		}

		u32 zone_query(const vk::Backend& b, const CommandList& list, u32 zone)
		{
			return ((list.slot * (b.ctx.max_lists + 1)) + list.index) * vk::MAX_ZONES * 2 +
			       zone * 2;
		}
	}

	namespace vk
	{
		StateInfo state_info(TextureState state, VkPipelineStageFlags2 shader_stages)
		{
			switch (state)
			{
				// The source stage pins to color output so a backbuffer's first transition orders
				// after the acquire semaphore. Costs nothing for other first uses.
				case TextureState::Undefined:
					return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
					    VK_IMAGE_LAYOUT_UNDEFINED};
				case TextureState::ColorTarget:
					return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
					        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
					    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
				case TextureState::DepthTarget:
					return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
					            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
					        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
					    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
				case TextureState::ShaderRead:
					return {shader_stages, VK_ACCESS_2_SHADER_READ_BIT,
					    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
				case TextureState::ShaderWrite:
					return {shader_stages,
					    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
					    VK_IMAGE_LAYOUT_GENERAL};
				case TextureState::CopySrc:
					return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
					    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
				case TextureState::CopyDst:
					return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
					    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};

				// No destination work: the present semaphore takes over.
				case TextureState::Present:
					return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
					    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
				case TextureState::Count:
					break;
			}

			EMBER_UNREACHABLE();
		}
	}

	void barrier(CommandList* list, Span<const TextureBarrier> textures,
	    Span<const BufferBarrier> buffers)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "barriers record outside render passes");

		vk::Backend& b = *vk::backend();
		const VkPipelineStageFlags2 shader_stages = b.ctx.all_shader_stages;

		constexpr u32 BATCH = 8;
		VkImageMemoryBarrier2 images[BATCH];
		VkBufferMemoryBarrier2 native_buffers[BATCH];
		u32 image_count = 0;
		u32 buffer_count = 0;

		const auto flush = [&]
		{
			const VkDependencyInfo dependency{
			    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			    .bufferMemoryBarrierCount = buffer_count,
			    .pBufferMemoryBarriers = native_buffers,
			    .imageMemoryBarrierCount = image_count,
			    .pImageMemoryBarriers = images,
			};
			vkCmdPipelineBarrier2(list->commands, &dependency);
			image_count = 0;
			buffer_count = 0;
		};

		for (const TextureBarrier& barrier : textures)
		{
			EMBER_ASSERT_MSG(barrier.after != TextureState::Undefined, "a transition into garbage");

			const vk::TextureHot* hot = b.textures.get(barrier.texture);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: a barrier names a stale texture");
				continue;
			}

			const vk::TextureCold& cold = b.texture_cold[barrier.texture.index()];
			const vk::StateInfo src = vk::state_info(barrier.before, shader_stages);
			const vk::StateInfo dst = vk::state_info(barrier.after, shader_stages);

			images[image_count++] = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			    .srcStageMask = src.stages,
			    .srcAccessMask = src.access,
			    .dstStageMask = dst.stages,
			    .dstAccessMask = dst.access,
			    .oldLayout = src.layout,
			    .newLayout = dst.layout,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .image = hot->image,
			    .subresourceRange =
			        {
			            vk::aspect_of(cold.api_format),
			            barrier.base_mip,
			            barrier.mip_count == ALL_MIPS ? VK_REMAINING_MIP_LEVELS : barrier.mip_count,
			            barrier.base_layer,
			            barrier.layer_count == ALL_LAYERS ? VK_REMAINING_ARRAY_LAYERS
			                                              : barrier.layer_count,
			        },
			};

			// The swapchain follows its acquired image's state, so end_frame knows what to move
			// to Present and what already is.
			if (!cold.swapchain.is_null())
			{
				if (vk::SwapchainData* data = b.swapchains.get(cold.swapchain);
				    data != nullptr && data->acquired_image == cold.backbuffer_index)
				{
					data->acquired_state = barrier.after;
				}
			}

			if (image_count == BATCH)
			{
				flush();
			}
		}

		// Buffers arrive with the buffer slice; until then a buffer barrier names nothing.
		for (const BufferBarrier& barrier : buffers)
		{
			(void)barrier;
			(void)buffer_state_info;
			EMBER_ERROR("gpu: buffer barriers need the buffer pool, which this build lacks");
		}

		if (image_count > 0 || buffer_count > 0)
		{
			flush();
		}
	}

	void memory_barrier(CommandList* list)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "barriers record outside render passes");

		const VkPipelineStageFlags2 all = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		const VkMemoryBarrier2 barrier{
		    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
		    .srcStageMask = all,
		    .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
		    .dstStageMask = all,
		    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
		};

		const VkDependencyInfo dependency{
		    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		    .memoryBarrierCount = 1,
		    .pMemoryBarriers = &barrier,
		};

		vkCmdPipelineBarrier2(list->commands, &dependency);
	}

	void begin_render_pass(CommandList* list, const RenderPassDef& def)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "a render pass is already open");
		EMBER_ASSERT(def.colors.size() <= MAX_COLOR_ATTACHMENTS);
		EMBER_ASSERT_MSG(!def.colors.empty() || !def.depth.texture.is_null(),
		    "a pass with nothing");

		vk::Backend& b = *vk::backend();
		VkRenderingAttachmentInfo colors[MAX_COLOR_ATTACHMENTS];
		VkExtent2D extent = {};
		u32 color_count = 0;

		for (const ColorAttachment& attachment : def.colors)
		{
			const vk::TextureHot* hot = b.textures.get(attachment.texture);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: a render pass names a stale color texture");
				return;
			}

			const vk::TextureCold& cold = b.texture_cold[attachment.texture.index()];
			const Extent3D size = mip_extent(
			    {cold.extent.width, cold.extent.height, cold.extent.depth}, attachment.mip);
			extent = {size.width, size.height};

			colors[color_count++] = {
			    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			    .imageView = attachment_view(*hot, cold, attachment.mip, attachment.layer),
			    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			    .loadOp = to_vk(attachment.load),
			    .storeOp = to_vk(attachment.store),
			    .clearValue = {.color = {.float32 = {attachment.clear.r, attachment.clear.g,
			                                 attachment.clear.b, attachment.clear.a}}},
			};
		}

		VkRenderingAttachmentInfo depth = {};
		const bool has_depth = !def.depth.texture.is_null();

		if (has_depth)
		{
			const vk::TextureHot* hot = b.textures.get(def.depth.texture);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: a render pass names a stale depth texture");
				return;
			}

			const vk::TextureCold& cold = b.texture_cold[def.depth.texture.index()];
			const Extent3D size = mip_extent(
			    {cold.extent.width, cold.extent.height, cold.extent.depth}, def.depth.mip);
			extent = {size.width, size.height};

			depth = {
			    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			    .imageView = attachment_view(*hot, cold, def.depth.mip, def.depth.layer),
			    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			    .loadOp = to_vk(def.depth.load),
			    .storeOp = to_vk(def.depth.store),
			    .clearValue = {.depthStencil = {def.depth.clear_depth, 0}},
			};
		}

		const VkRenderingInfo rendering{
		    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		    .renderArea = {{0, 0}, extent},
		    .layerCount = 1,
		    .colorAttachmentCount = color_count,
		    .pColorAttachments = colors,
		    .pDepthAttachment = has_depth ? &depth : nullptr,
		};

		vkCmdBeginRendering(list->commands, &rendering);
		list->inside_pass = true;

		set_viewport(list, {.width = (f32)extent.width, .height = (f32)extent.height});
		set_scissor(list, {.width = extent.width, .height = extent.height});
	}

	void end_render_pass(CommandList* list)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(list->inside_pass, "no render pass is open");
		vkCmdEndRendering(list->commands);
		list->inside_pass = false;
	}

	void set_viewport(CommandList* list, const Viewport& viewport)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(list->inside_pass, "the viewport is set inside a render pass");
		EMBER_ASSERT(viewport.width > 0.0f && viewport.height > 0.0f);

		const VkViewport native = flip(viewport);
		vkCmdSetViewport(list->commands, 0, 1, &native);
	}

	void set_scissor(CommandList* list, const Rect& scissor)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(list->inside_pass, "the scissor is set inside a render pass");
		EMBER_ASSERT(scissor.x >= 0 && scissor.y >= 0);

		// Scissors live in framebuffer coordinates on every backend; the flip does not touch them.
		const VkRect2D native{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
		vkCmdSetScissor(list->commands, 0, 1, &native);
	}

	void begin_zone(CommandList* list, const char* name, u32 color)
	{
		if (!recording(list))
		{
			return;
		}

		vk::Backend& b = *vk::backend();

		if (b.ctx.debug_utils)
		{
			const VkDebugUtilsLabelEXT label{
			    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			    .pLabelName = name,
			    .color = {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
			        (f32)(color & 0xff) / 255.0f, 1.0f},
			};
			vkCmdBeginDebugUtilsLabelEXT(list->commands, &label);
		}

		// Past the cap the zone still labels and nests; it just goes untimed.
		u8 claimed = vk::ZONE_UNTIMED;
		if (b.timestamps != VK_NULL_HANDLE && list->zone_count < vk::MAX_ZONES)
		{
			claimed = (u8)list->zone_count++;
			vkCmdWriteTimestamp2(list->commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.timestamps,
			    zone_query(b, *list, claimed));
			list->zones[claimed] = {.name = name, .color = color, .depth = list->zone_depth};
		}

		if (list->zone_depth < vk::MAX_ZONES)
		{
			list->zone_stack[list->zone_depth] = claimed;
		}

		++list->zone_depth;
	}

	void end_zone(CommandList* list)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(list->zone_depth > 0, "end_zone without begin_zone");
		vk::Backend& b = *vk::backend();

		--list->zone_depth;

		// Close the zone this depth opened; an untimed zone wrote no begin to pair with.
		if (list->zone_depth < vk::MAX_ZONES)
		{
			const u8 claimed = list->zone_stack[list->zone_depth];
			if (claimed != vk::ZONE_UNTIMED)
			{
				vkCmdWriteTimestamp2(list->commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				    b.timestamps, zone_query(b, *list, claimed) + 1);
			}
		}

		if (b.ctx.debug_utils)
		{
			vkCmdEndDebugUtilsLabelEXT(list->commands);
		}
	}
}
