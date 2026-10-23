// Records public GPU commands into the command buffer owned by each list. Handle lookups take
// no lock, and each list has one writer. Calls made outside recording assert in debug builds
// and otherwise return.
//
// Pipeline and index-buffer binds compare against the list's current bindings so repeated
// binds can be skipped. set_constants() copies the data now and leaves the descriptor bind
// until the next draw or dispatch. Graphics and compute each flush their own pending offsets.

#include <ember/gpu/commands.h>

#include <gpu/vulkan/backend.h>

#include <cstring>

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

				// Fills and copies use different transfer stages. Cover both so the same states
				// work for either command.
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

		// Reuse the main view for a single subresource. Other attachments need the mip and layer
		// views prepared when the texture was created.
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

		// Callers use a top-left origin and positive height. Moving the origin to the bottom and
		// negating the height gives Vulkan the same viewport while keeping clip-space Y up.
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

		// Remember the state a list leaves an acquired backbuffer in, one entry per swapchain. A
		// later barrier on the same swapchain's image replaces the earlier entry.
		void note_backbuffer(CommandList& list, SwapchainHandle swapchain, u32 image,
		    TextureState state)
		{
			for (u32 i = 0; i < list.backbuffer_count; ++i)
			{
				CommandList::BackbufferUse& use = list.backbuffers[i];
				if (use.swapchain == swapchain)
				{
					use.image = image;
					use.state = state;
					return;
				}
			}

			if (list.backbuffer_count < vk::MAX_SWAPCHAINS)
			{
				list.backbuffers[list.backbuffer_count++] = {swapchain, image, state};
			}
		}

		// Integer attachments clear through the union's integer view. Writing floats there would
		// store the float's bit pattern: 1.0f becomes 1065353216.
		VkClearColorValue clear_value(const ClearColor& clear, bool integer)
		{
			VkClearColorValue value;
			if (integer)
			{
				value.uint32[0] = (u32)(i64)clear.r;
				value.uint32[1] = (u32)(i64)clear.g;
				value.uint32[2] = (u32)(i64)clear.b;
				value.uint32[3] = (u32)(i64)clear.a;
			}
			else
			{
				value.float32[0] = clear.r;
				value.float32[1] = clear.g;
				value.float32[2] = clear.b;
				value.float32[3] = clear.a;
			}

			return value;
		}

		u32 zone_query(const vk::Backend& b, const CommandList& list, u32 zone)
		{
			return ((list.slot * (b.ctx.list_total + 1)) + list.index) * vk::MAX_ZONES * 2 +
			       zone * 2;
		}

		// Vulkan keeps graphics and compute descriptor bindings separately. Bind the latest
		// constant offsets for one bind point and clear its dirty flag. Kept out of line so a draw
		// whose constants are unchanged is three checks and a tail call into Vulkan, with no
		// register spills for this path.
		EMBER_NOINLINE void flush_constants(CommandList* list, VkPipelineBindPoint bind_point,
		    bool& dirty)
		{
			const vk::Backend& b = *vk::backend();
			vkCmdBindDescriptorSets(list->commands, bind_point, b.heap.pipeline_layout, 1, 1,
			    &b.heap.constants, CONSTANT_SLOTS, list->constant_offsets);
			dirty = false;
		}

		// Every draw needs these recording checks and the latest constant bindings. The pass and
		// pipeline checks are assertions, so callers must satisfy them in every build. Only a
		// changed constant slot costs the backend lookup; the clean path goes straight to the draw.
		bool draw_ready(CommandList* list)
		{
			if (!recording(list))
			{
				return false;
			}

			EMBER_ASSERT_MSG(list->inside_pass, "draws record inside a render pass");
			EMBER_ASSERT_MSG(!list->graphics_pipeline.is_null(), "a draw with no pipeline bound");
			if (list->constants_dirty_graphics)
			{
				flush_constants(list, VK_PIPELINE_BIND_POINT_GRAPHICS,
				    list->constants_dirty_graphics);
			}
			return true;
		}

		bool dispatch_ready(CommandList* list)
		{
			if (!recording(list))
			{
				return false;
			}

			EMBER_ASSERT_MSG(!list->inside_pass, "dispatches record outside render passes");
			EMBER_ASSERT_MSG(list->queue != Queue::Transfer, "a dispatch on the transfer queue");
			EMBER_ASSERT_MSG(!list->compute_pipeline.is_null(),
			    "a dispatch with no pipeline bound");
			if (list->constants_dirty_compute)
			{
				flush_constants(list, VK_PIPELINE_BIND_POINT_COMPUTE,
				    list->constants_dirty_compute);
			}
			return true;
		}

		// Resolve the argument buffer once for the indirect commands. A stale handle logs and
		// returns null, letting the caller skip the command.
		VkBuffer indirect_buffer(const vk::Backend& b, BufferHandle handle, u64 offset)
		{
			const vk::BufferHot* hot = b.buffers.get(handle);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: an indirect command names a stale buffer");
				return VK_NULL_HANDLE;
			}

			EMBER_ASSERT_MSG(has_any(b.buffer_cold[handle.index()].usage, BufferUsage::Indirect),
			    "indirect arguments in a buffer without BufferUsage::Indirect");
			EMBER_ASSERT(offset % 4 == 0);
			return hot->buffer;
		}
	}

	namespace vk
	{
		StateInfo state_info(TextureState state, VkPipelineStageFlags2 shader_stages)
		{
			switch (state)
			{
				// Discarding contents leaves no old accesses to make visible and nothing to wait
				// for, on any queue. An image whose last use was a read names that state instead,
				// so the new writes are ordered after it; a backbuffer's acquire wait covers it.
				case TextureState::Undefined:
					return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED};
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

				// Presentation has no destination stage here. The submission's signal semaphore
				// carries the handoff to the presentation engine.
				case TextureState::Present:
					return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
					    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
				case TextureState::Count:
					break;
			}

			EMBER_UNREACHABLE();
		}

		VkImageLayout layout_of(const TextureCold& cold, TextureState state)
		{
			return state == TextureState::ShaderRead
			           ? read_layout_of(cold.usage)
			           : state_info(state, VK_PIPELINE_STAGE_2_NONE).layout;
		}

		VkPipelineStageFlags2 shader_stages_of(const Context& ctx, Queue queue)
		{
			switch (queue)
			{
				case Queue::Graphics:
					return ctx.all_shader_stages;
				case Queue::Compute:
					return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
				case Queue::Transfer:
				case Queue::Count:
					break;
			}

			return VK_PIPELINE_STAGE_2_NONE;
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
		const VkPipelineStageFlags2 shader_stages = vk::shader_stages_of(b.ctx, list->queue);

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
			EMBER_ASSERT(barrier.from < Queue::Count && barrier.to < Queue::Count);

			const vk::TextureHot* hot = b.textures.get(barrier.texture);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: a barrier names a stale texture");
				continue;
			}

			const vk::TextureCold& cold = b.texture_cold[barrier.texture.index()];
			EMBER_ASSERT_MSG((barrier.before != TextureState::ShaderRead &&
			                     barrier.after != TextureState::ShaderRead) ||
			                     has_any(cold.usage, TextureUsage::Sampled | TextureUsage::Storage),
			    "ShaderRead on a texture no shader can read");

			// An ownership transfer is two halves of one barrier: a release on the giving queue
			// and an acquire on the taking one, with the same layouts. Queues of one family own
			// nothing separately, so there the release half is an ordinary barrier and the
			// acquire half records nothing.
			const u32 from_family = vk::queue_for(b, barrier.from).family;
			const u32 to_family = vk::queue_for(b, barrier.to).family;
			const bool transfer = from_family != to_family;
			EMBER_ASSERT_MSG(!transfer || list->queue == barrier.from || list->queue == barrier.to,
			    "an ownership transfer recorded on a queue it does not involve");
			if (!transfer && barrier.from != barrier.to && list->queue == barrier.to)
			{
				continue;
			}

			vk::StateInfo src = vk::state_info(barrier.before, shader_stages);
			vk::StateInfo dst = vk::state_info(barrier.after, shader_stages);
			src.layout = vk::layout_of(cold, barrier.before);
			dst.layout = vk::layout_of(cold, barrier.after);

			// The scope on the far side of a transfer does not apply, and it may name stages this
			// queue lacks, so leave it empty: the submission wait between the halves orders them.
			if (transfer && list->queue == barrier.from)
			{
				dst.stages = VK_PIPELINE_STAGE_2_NONE;
				dst.access = VK_ACCESS_2_NONE;
			}
			if (transfer && list->queue == barrier.to)
			{
				src.stages = VK_PIPELINE_STAGE_2_NONE;
				src.access = VK_ACCESS_2_NONE;
			}

			images[image_count++] = {
			    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			    .srcStageMask = src.stages,
			    .srcAccessMask = src.access,
			    .dstStageMask = dst.stages,
			    .dstAccessMask = dst.access,
			    .oldLayout = src.layout,
			    .newLayout = dst.layout,
			    .srcQueueFamilyIndex = transfer ? from_family : VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = transfer ? to_family : VK_QUEUE_FAMILY_IGNORED,
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

			// Remember the state this list leaves an acquired backbuffer in. submit() folds it
			// into the frame's pending present in submission order; no shared state changes here.
			if (!cold.swapchain.is_null())
			{
				EMBER_ASSERT_MSG(list->queue == Queue::Graphics,
				    "a backbuffer on a queue that cannot present");
				note_backbuffer(*list, cold.swapchain, cold.backbuffer_index, barrier.after);
			}

			if (image_count == BATCH)
			{
				flush();
			}
		}

		for (const BufferBarrier& barrier : buffers)
		{
			const vk::BufferHot* hot = b.buffers.get(barrier.buffer);
			if (hot == nullptr)
			{
				EMBER_ERROR("gpu: a barrier names a stale buffer");
				continue;
			}

			const BufferStateInfo src = buffer_state_info(barrier.before, shader_stages);
			const BufferStateInfo dst = buffer_state_info(barrier.after, shader_stages);

			native_buffers[buffer_count++] = {
			    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
			    .srcStageMask = src.stages,
			    .srcAccessMask = src.access,
			    .dstStageMask = dst.stages,
			    .dstAccessMask = dst.access,
			    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			    .buffer = hot->buffer,
			    .offset = 0,
			    .size = VK_WHOLE_SIZE,
			};

			if (buffer_count == BATCH)
			{
				flush();
			}
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
		EMBER_ASSERT_MSG(list->queue == Queue::Graphics,
		    "a render pass on a queue that cannot draw");
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
			    .clearValue = {.color = clear_value(attachment.clear,
			                       format_info(cold.api_format).integer)},
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

		// Scissors already use framebuffer coordinates, so only the viewport needs the Y flip.
		const VkRect2D native{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
		vkCmdSetScissor(list->commands, 0, 1, &native);
	}

	void bind_pipeline(CommandList* list, GraphicsPipelineHandle pipeline)
	{
		if (!recording(list) || list->graphics_pipeline == pipeline)
		{
			return;
		}

		const vk::PipelineData* data = vk::backend()->graphics_pipelines.get(pipeline);
		if (data == nullptr)
		{
			EMBER_ERROR("gpu: a bind names a stale graphics pipeline");
			return;
		}

		vkCmdBindPipeline(list->commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
		    data->pipeline.load(std::memory_order_acquire));
		list->graphics_pipeline = pipeline;
	}

	void bind_pipeline(CommandList* list, ComputePipelineHandle pipeline)
	{
		if (!recording(list) || list->compute_pipeline == pipeline)
		{
			return;
		}

		const vk::PipelineData* data = vk::backend()->compute_pipelines.get(pipeline);
		if (data == nullptr)
		{
			EMBER_ERROR("gpu: a bind names a stale compute pipeline");
			return;
		}

		vkCmdBindPipeline(list->commands, VK_PIPELINE_BIND_POINT_COMPUTE,
		    data->pipeline.load(std::memory_order_acquire));
		list->compute_pipeline = pipeline;
	}

	void push_constants(CommandList* list, const void* data, u32 size)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT(size != 0 && size <= PUSH_CONSTANT_BYTES && size % 4 == 0);
		EMBER_ASSERT_MSG(list->queue != Queue::Transfer, "push constants on the transfer queue");

		// Every pipeline shares this layout, so push constants can be recorded before a bind.
		vkCmdPushConstants(list->commands, vk::backend()->heap.pipeline_layout, VK_SHADER_STAGE_ALL,
		    0, size, data);
	}

	void set_constants(CommandList* list, u32 slot, const void* data, u32 size)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT(slot < CONSTANT_SLOTS && size != 0 && size <= MAX_CONSTANT_BLOCK_BYTES);

		vk::Backend& b = *vk::backend();
		const vk::RingSpan span =
		    vk::ring_allocate(b.transient, list->slot, size, b.ctx.caps.constant_offset_alignment);
		if (span.cpu == nullptr)
		{
			return;
		}

		std::memcpy(span.cpu, data, size);
		list->constant_offsets[slot] = (u32)span.offset;
		list->constants_dirty_graphics = true;
		list->constants_dirty_compute = true;
	}

	void bind_index_buffer(CommandList* list, BufferHandle buffer, IndexFormat format, u64 offset)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT(format < IndexFormat::Count);

		const vk::Backend& b = *vk::backend();
		const vk::BufferHot* hot = b.buffers.get(buffer);
		if (hot == nullptr)
		{
			EMBER_ERROR("gpu: a bind names a stale index buffer");
			return;
		}

		EMBER_ASSERT_MSG(has_any(b.buffer_cold[buffer.index()].usage, BufferUsage::Index),
		    "an index buffer without BufferUsage::Index");

		const VkIndexType type =
		    format == IndexFormat::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
		if (hot->buffer == list->index_buffer && offset == list->index_offset &&
		    type == list->index_type)
		{
			return;
		}

		vkCmdBindIndexBuffer(list->commands, hot->buffer, offset, type);
		list->index_buffer = hot->buffer;
		list->index_offset = offset;
		list->index_type = type;
	}

	void draw(CommandList* list, u32 vertex_count, u32 instance_count, u32 first_vertex,
	    u32 first_instance)
	{
		if (!draw_ready(list))
		{
			return;
		}

		vkCmdDraw(list->commands, vertex_count, instance_count, first_vertex, first_instance);
	}

	void draw_indexed(CommandList* list, u32 index_count, u32 instance_count, u32 first_index,
	    i32 base_vertex, u32 first_instance)
	{
		if (!draw_ready(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(list->index_buffer != VK_NULL_HANDLE,
		    "an indexed draw with no index buffer bound");
		vkCmdDrawIndexed(list->commands, index_count, instance_count, first_index, base_vertex,
		    first_instance);
	}

	void draw_indirect(CommandList* list, BufferHandle args, u64 offset, u32 draw_count, u32 stride)
	{
		if (!draw_ready(list))
		{
			return;
		}

		const vk::Backend& b = *vk::backend();
		EMBER_ASSERT(stride % 4 == 0 && stride >= sizeof(DrawIndirectArgs));
		EMBER_ASSERT(draw_count <= b.ctx.caps.max_indirect_draw_count);

		if (const VkBuffer buffer = indirect_buffer(b, args, offset); buffer != VK_NULL_HANDLE)
		{
			vkCmdDrawIndirect(list->commands, buffer, offset, draw_count, stride);
		}
	}

	void draw_indexed_indirect(CommandList* list, BufferHandle args, u64 offset, u32 draw_count,
	    u32 stride)
	{
		if (!draw_ready(list))
		{
			return;
		}

		const vk::Backend& b = *vk::backend();
		EMBER_ASSERT_MSG(list->index_buffer != VK_NULL_HANDLE,
		    "an indexed draw with no index buffer bound");
		EMBER_ASSERT(stride % 4 == 0 && stride >= sizeof(DrawIndexedIndirectArgs));
		EMBER_ASSERT(draw_count <= b.ctx.caps.max_indirect_draw_count);

		if (const VkBuffer buffer = indirect_buffer(b, args, offset); buffer != VK_NULL_HANDLE)
		{
			vkCmdDrawIndexedIndirect(list->commands, buffer, offset, draw_count, stride);
		}
	}

	void draw_indirect_count(CommandList* list, BufferHandle args, u64 offset, BufferHandle count,
	    u64 count_offset, u32 max_draw_count, u32 stride)
	{
		if (!draw_ready(list))
		{
			return;
		}

		const vk::Backend& b = *vk::backend();
		EMBER_ASSERT_MSG(b.ctx.caps.indirect_count,
		    "draw_indirect_count needs Caps::indirect_count");
		EMBER_ASSERT(stride % 4 == 0 && stride >= sizeof(DrawIndirectArgs));
		EMBER_ASSERT(max_draw_count <= b.ctx.caps.max_indirect_draw_count);

		const VkBuffer args_buffer = indirect_buffer(b, args, offset);
		const VkBuffer count_buffer = indirect_buffer(b, count, count_offset);
		if (b.ctx.caps.indirect_count && args_buffer != VK_NULL_HANDLE &&
		    count_buffer != VK_NULL_HANDLE)
		{
			vkCmdDrawIndirectCount(list->commands, args_buffer, offset, count_buffer, count_offset,
			    max_draw_count, stride);
		}
	}

	void draw_indexed_indirect_count(CommandList* list, BufferHandle args, u64 offset,
	    BufferHandle count, u64 count_offset, u32 max_draw_count, u32 stride)
	{
		if (!draw_ready(list))
		{
			return;
		}

		const vk::Backend& b = *vk::backend();
		EMBER_ASSERT_MSG(b.ctx.caps.indirect_count,
		    "draw_indirect_count needs Caps::indirect_count");
		EMBER_ASSERT_MSG(list->index_buffer != VK_NULL_HANDLE,
		    "an indexed draw with no index buffer bound");
		EMBER_ASSERT(stride % 4 == 0 && stride >= sizeof(DrawIndexedIndirectArgs));
		EMBER_ASSERT(max_draw_count <= b.ctx.caps.max_indirect_draw_count);

		const VkBuffer args_buffer = indirect_buffer(b, args, offset);
		const VkBuffer count_buffer = indirect_buffer(b, count, count_offset);
		if (b.ctx.caps.indirect_count && args_buffer != VK_NULL_HANDLE &&
		    count_buffer != VK_NULL_HANDLE)
		{
			vkCmdDrawIndexedIndirectCount(list->commands, args_buffer, offset, count_buffer,
			    count_offset, max_draw_count, stride);
		}
	}

	void dispatch(CommandList* list, u32 x, u32 y, u32 z)
	{
		if (!dispatch_ready(list))
		{
			return;
		}

		vkCmdDispatch(list->commands, x, y, z);
	}

	void dispatch_indirect(CommandList* list, BufferHandle args, u64 offset)
	{
		if (!dispatch_ready(list))
		{
			return;
		}

		if (const VkBuffer buffer = indirect_buffer(*vk::backend(), args, offset);
		    buffer != VK_NULL_HANDLE)
		{
			vkCmdDispatchIndirect(list->commands, buffer, offset);
		}
	}

	void copy_buffer(CommandList* list, BufferHandle src, u64 src_offset, BufferHandle dst,
	    u64 dst_offset, u64 size)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "copies record outside render passes");

		vk::Backend& b = *vk::backend();
		const vk::BufferHot* source = b.buffers.get(src);
		const vk::BufferHot* target = b.buffers.get(dst);
		if (source == nullptr || target == nullptr)
		{
			EMBER_ERROR("gpu: a copy names a stale buffer");
			return;
		}

		const VkBufferCopy2 region{
		    .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
		    .srcOffset = src_offset,
		    .dstOffset = dst_offset,
		    .size = size,
		};
		const VkCopyBufferInfo2 copy{
		    .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
		    .srcBuffer = source->buffer,
		    .dstBuffer = target->buffer,
		    .regionCount = 1,
		    .pRegions = &region,
		};
		vkCmdCopyBuffer2(list->commands, &copy);
	}

	void copy_buffer_to_texture(CommandList* list, BufferHandle src, u64 src_offset,
	    TextureHandle dst, u32 mip, u32 layer)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "copies record outside render passes");

		vk::Backend& b = *vk::backend();
		const vk::BufferHot* source = b.buffers.get(src);
		const vk::TextureHot* target = b.textures.get(dst);
		if (source == nullptr || target == nullptr)
		{
			EMBER_ERROR("gpu: a copy names a stale buffer or texture");
			return;
		}

		const vk::TextureCold& cold = b.texture_cold[dst.index()];
		EMBER_ASSERT(mip < cold.mip_count && layer < cold.layer_count);

		const Extent3D size =
		    mip_extent({cold.extent.width, cold.extent.height, cold.extent.depth}, mip);
		const VkBufferImageCopy2 region{
		    .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		    .bufferOffset = src_offset,
		    .imageSubresource = {vk::aspect_of(cold.api_format), mip, layer, 1},
		    .imageExtent = {size.width, size.height, size.depth},
		};
		const VkCopyBufferToImageInfo2 copy{
		    .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		    .srcBuffer = source->buffer,
		    .dstImage = target->image,
		    .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .regionCount = 1,
		    .pRegions = &region,
		};
		vkCmdCopyBufferToImage2(list->commands, &copy);
	}

	void copy_texture_to_buffer(CommandList* list, TextureHandle src, u32 mip, u32 layer,
	    BufferHandle dst, u64 dst_offset)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "copies record outside render passes");

		vk::Backend& b = *vk::backend();
		const vk::TextureHot* source = b.textures.get(src);
		const vk::BufferHot* target = b.buffers.get(dst);
		if (source == nullptr || target == nullptr)
		{
			EMBER_ERROR("gpu: a copy names a stale buffer or texture");
			return;
		}

		const vk::TextureCold& cold = b.texture_cold[src.index()];
		EMBER_ASSERT(mip < cold.mip_count && layer < cold.layer_count);

		const Extent3D size =
		    mip_extent({cold.extent.width, cold.extent.height, cold.extent.depth}, mip);
		const VkBufferImageCopy2 region{
		    .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		    .bufferOffset = dst_offset,
		    .imageSubresource = {vk::aspect_of(cold.api_format), mip, layer, 1},
		    .imageExtent = {size.width, size.height, size.depth},
		};
		const VkCopyImageToBufferInfo2 copy{
		    .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
		    .srcImage = source->image,
		    .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    .dstBuffer = target->buffer,
		    .regionCount = 1,
		    .pRegions = &region,
		};
		vkCmdCopyImageToBuffer2(list->commands, &copy);
	}

	void fill_buffer(CommandList* list, BufferHandle dst, u64 offset, u64 size, u32 value)
	{
		if (!recording(list))
		{
			return;
		}

		EMBER_ASSERT_MSG(!list->inside_pass, "fills record outside render passes");
		EMBER_ASSERT(offset % 4 == 0 && size % 4 == 0);

		const vk::BufferHot* target = vk::backend()->buffers.get(dst);
		if (target == nullptr)
		{
			EMBER_ERROR("gpu: a fill names a stale buffer");
			return;
		}

		vkCmdFillBuffer(list->commands, target->buffer, offset, size, value);
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

		// Keep debug labels and nesting after the timestamp slots run out, and on a queue whose
		// family cannot write timestamps. Only timing is lost.
		u8 claimed = vk::ZONE_UNTIMED;
		if (b.timestamps != VK_NULL_HANDLE && vk::queue_for(b, list->queue).timestamps &&
		    list->zone_count < vk::MAX_ZONES)
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

		// Match the timestamp to the zone opened at this depth. Untimed zones have no query to end.
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
