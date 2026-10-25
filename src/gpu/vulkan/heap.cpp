// All pipelines share the bindless arrays, dynamic constants and layout. Unused slots start
// with fallbacks; reclamation restores them before reuse. Stale indices can name later resources.

#include <ember/gpu/gpu.h>

#include <gpu/vulkan/backend.h>

namespace ember::gpu::vk
{
	namespace
	{
		constexpr VkDescriptorBindingFlags BINDLESS_FLAGS =
		    VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
		    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

		constexpr u32 BINDING_COUNT = 9;

		void write_image(const Context& ctx, VkDescriptorSet set, u32 binding, u32 slot,
		    VkDescriptorType type, VkImageView view, VkSampler sampler, VkImageLayout layout)
		{
			const VkDescriptorImageInfo info{sampler, view, layout};
			const VkWriteDescriptorSet write{
			    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			    .dstSet = set,
			    .dstBinding = binding,
			    .dstArrayElement = slot,
			    .descriptorCount = 1,
			    .descriptorType = type,
			    .pImageInfo = &info,
			};
			vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
		}

		void write_buffer(const Context& ctx, VkDescriptorSet set, u32 binding, u32 slot,
		    VkDescriptorType type, VkBuffer buffer, u64 offset, u64 range)
		{
			const VkDescriptorBufferInfo info{buffer, offset, range};
			const VkWriteDescriptorSet write{
			    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			    .dstSet = set,
			    .dstBinding = binding,
			    .dstArrayElement = slot,
			    .descriptorCount = 1,
			    .descriptorType = type,
			    .pBufferInfo = &info,
			};
			vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
		}

		// Fill the binding in fixed-size batches so stack usage does not grow with heap capacity.
		void flood(const Context& ctx, VkDescriptorSet set, u32 binding, u32 count,
		    VkDescriptorType type, const VkDescriptorImageInfo* image,
		    const VkDescriptorBufferInfo* buffer)
		{
			constexpr u32 RUN = 256;
			VkDescriptorImageInfo images[RUN];
			VkDescriptorBufferInfo buffers[RUN];

			for (u32 i = 0; i < RUN; ++i)
			{
				if (image != nullptr)
				{
					images[i] = *image;
				}
				else
				{
					buffers[i] = *buffer;
				}
			}

			for (u32 first = 0; first < count; first += RUN)
			{
				const u32 run = count - first < RUN ? count - first : RUN;
				const VkWriteDescriptorSet write{
				    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				    .dstSet = set,
				    .dstBinding = binding,
				    .dstArrayElement = first,
				    .descriptorCount = run,
				    .descriptorType = type,
				    .pImageInfo = image != nullptr ? images : nullptr,
				    .pBufferInfo = buffer != nullptr ? buffers : nullptr,
				};
				vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
			}
		}

		// Initialize fallback resources through a temporary graphics command buffer, then wait
		// before releasing its pool. This runs during boot, where blocking is allowed.
		template <typename F>
		bool boot_commands(Backend& b, F record)
		{
			const Context& ctx = b.ctx;
			QueueState& queue = queue_for(b, Queue::Graphics);

			const VkCommandPoolCreateInfo pool_info{
			    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			    .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			    .queueFamilyIndex = queue.family,
			};

			VkCommandPool pool = VK_NULL_HANDLE;
			if (vkCreateCommandPool(ctx.device, &pool_info, &ctx.callbacks, &pool) != VK_SUCCESS)
			{
				EMBER_ERROR("vulkan: boot command pool creation failed");
				return false;
			}

			const VkCommandBufferAllocateInfo buffer_info{
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			    .commandPool = pool,
			    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			    .commandBufferCount = 1,
			};

			VkCommandBuffer commands = VK_NULL_HANDLE;
			bool ok = vkAllocateCommandBuffers(ctx.device, &buffer_info, &commands) == VK_SUCCESS;

			if (ok)
			{
				const VkCommandBufferBeginInfo begin{
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				};
				EMBER_VK_CHECK(vkBeginCommandBuffer(commands, &begin));
				record(commands);
				EMBER_VK_CHECK(vkEndCommandBuffer(commands));

				const VkCommandBufferSubmitInfo submit_commands{
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				    .commandBuffer = commands,
				};
				const VkSubmitInfo2 submit{
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				    .commandBufferInfoCount = 1,
				    .pCommandBufferInfos = &submit_commands,
				};

				queue.lock.lock();
				ok = vkQueueSubmit2(queue.handle, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
				ok = ok && vkQueueWaitIdle(queue.handle) == VK_SUCCESS;
				queue.lock.unlock();
			}

			vkDestroyCommandPool(ctx.device, pool, &ctx.callbacks);
			if (!ok)
			{
				EMBER_ERROR("vulkan: the boot commands failed");
			}

			return ok;
		}
	}

	bool heap_boot(Backend& b)
	{
		const Context& ctx = b.ctx;
		Heap& heap = b.heap;
		const u32 textures = b.def.limits.max_textures;
		const u32 samplers = b.def.limits.max_samplers;
		const u32 buffers = b.def.limits.max_buffers;

		const VkDescriptorSetLayoutBinding bindings[BINDING_COUNT] = {
		    {BINDING_SAMPLED_2D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textures, VK_SHADER_STAGE_ALL,
		        nullptr},
		    {BINDING_SAMPLED_2D_ARRAY, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textures,
		        VK_SHADER_STAGE_ALL, nullptr},
		    {BINDING_SAMPLED_CUBE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textures, VK_SHADER_STAGE_ALL,
		        nullptr},
		    {BINDING_SAMPLED_3D, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textures, VK_SHADER_STAGE_ALL,
		        nullptr},
		    {BINDING_STORAGE_2D, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textures, VK_SHADER_STAGE_ALL,
		        nullptr},
		    {BINDING_STORAGE_2D_ARRAY, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textures,
		        VK_SHADER_STAGE_ALL, nullptr},
		    {BINDING_STORAGE_3D, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textures, VK_SHADER_STAGE_ALL,
		        nullptr},
		    {BINDING_SAMPLERS, VK_DESCRIPTOR_TYPE_SAMPLER, samplers, VK_SHADER_STAGE_ALL, nullptr},
		    {BINDING_STORAGE_BUFFERS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffers,
		        VK_SHADER_STAGE_ALL, nullptr},
		};

		VkDescriptorBindingFlags flags[BINDING_COUNT];
		for (VkDescriptorBindingFlags& flag : flags)
		{
			flag = BINDLESS_FLAGS;
		}

		const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
		    .bindingCount = BINDING_COUNT,
		    .pBindingFlags = flags,
		};

		const VkDescriptorSetLayoutCreateInfo heap_info{
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .pNext = &flags_info,
		    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
		    .bindingCount = BINDING_COUNT,
		    .pBindings = bindings,
		};

		if (vkCreateDescriptorSetLayout(ctx.device, &heap_info, &ctx.callbacks,
		        &heap.heap_layout) != VK_SUCCESS)
		{
			EMBER_ERROR("vulkan: the bindless heap layout failed");
			return false;
		}

		// Set 1 always points at the transient buffer. Each draw selects its constant blocks
		// through dynamic offsets, without rewriting these descriptors.
		VkDescriptorSetLayoutBinding constant_bindings[CONSTANT_SLOTS];
		for (u32 i = 0; i < CONSTANT_SLOTS; ++i)
		{
			constant_bindings[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
			    VK_SHADER_STAGE_ALL, nullptr};
		}

		const VkDescriptorSetLayoutCreateInfo constants_info{
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		    .bindingCount = CONSTANT_SLOTS,
		    .pBindings = constant_bindings,
		};

		if (vkCreateDescriptorSetLayout(ctx.device, &constants_info, &ctx.callbacks,
		        &heap.constants_layout) != VK_SUCCESS)
		{
			EMBER_ERROR("vulkan: the constants layout failed");
			return false;
		}

		// A shared layout keeps descriptors and push constants compatible across pipeline binds.
		const VkDescriptorSetLayout set_layouts[] = {heap.heap_layout, heap.constants_layout};
		const VkPushConstantRange push_range{
		    .stageFlags = VK_SHADER_STAGE_ALL,
		    .offset = 0,
		    .size = PUSH_CONSTANT_BYTES,
		};

		const VkPipelineLayoutCreateInfo layout_info{
		    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		    .setLayoutCount = 2,
		    .pSetLayouts = set_layouts,
		    .pushConstantRangeCount = 1,
		    .pPushConstantRanges = &push_range,
		};

		if (vkCreatePipelineLayout(ctx.device, &layout_info, &ctx.callbacks,
		        &heap.pipeline_layout) != VK_SUCCESS)
		{
			EMBER_ERROR("vulkan: the pipeline layout failed");
			return false;
		}

		const VkDescriptorPoolSize sizes[] = {
		    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, textures * SAMPLED_ARRAY_COUNT},
		    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, textures * STORAGE_ARRAY_COUNT},
		    {VK_DESCRIPTOR_TYPE_SAMPLER, samplers},
		    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffers},
		    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, CONSTANT_SLOTS},
		};

		const VkDescriptorPoolCreateInfo pool_info{
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		    .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
		    .maxSets = 2,
		    .poolSizeCount = (u32)(sizeof(sizes) / sizeof(sizes[0])),
		    .pPoolSizes = sizes,
		};

		if (vkCreateDescriptorPool(ctx.device, &pool_info, &ctx.callbacks, &heap.pool) !=
		    VK_SUCCESS)
		{
			EMBER_ERROR("vulkan: the descriptor pool failed");
			return false;
		}

		const VkDescriptorSetAllocateInfo set_info{
		    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		    .descriptorPool = heap.pool,
		    .descriptorSetCount = 2,
		    .pSetLayouts = set_layouts,
		};

		VkDescriptorSet sets[2] = {};
		if (vkAllocateDescriptorSets(ctx.device, &set_info, sets) != VK_SUCCESS)
		{
			EMBER_ERROR("vulkan: the descriptor sets failed");
			return false;
		}

		heap.set = sets[0];
		heap.constants = sets[1];
		set_name(ctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, (u64)heap.set, "ember.heap");
		set_name(ctx, VK_OBJECT_TYPE_DESCRIPTOR_SET, (u64)heap.constants, "ember.constants");
		return true;
	}

	bool heap_fallbacks(Backend& b)
	{
		const Context& ctx = b.ctx;
		Heap& heap = b.heap;

		// Create fallbacks before user resources. Each texture type needs a matching sampled
		// view, and storage views let the same images fill the storage arrays too.
		constexpr TextureDef DEFS[(u32)TextureType::Count] = {
		    {.name = "ember.fallback.2d", .type = TextureType::Texture2D},
		    {.name = "ember.fallback.2d_array", .type = TextureType::Texture2DArray},
		    {.name = "ember.fallback.cube", .type = TextureType::TextureCube, .layers = 6},
		    {.name = "ember.fallback.3d", .type = TextureType::Texture3D},
		};

		for (u32 i = 0; i < (u32)TextureType::Count; ++i)
		{
			TextureDef def = DEFS[i];
			def.format = TextureFormat::RGBA8Unorm;
			def.usage = TextureUsage::Sampled | TextureUsage::Storage;
			heap.fallback_textures[i] = create_texture(def);
			if (heap.fallback_textures[i].is_null())
			{
				return false;
			}
		}

		heap.fallback_sampler = create_sampler({.name = "ember.fallback.sampler"});
		heap.fallback_buffer = create_buffer({
		    .name = "ember.fallback.buffer",
		    .size = 256,
		    .usage = BufferUsage::Storage,
		});

		if (heap.fallback_sampler.is_null() || heap.fallback_buffer.is_null())
		{
			return false;
		}

		EMBER_ASSERT_MSG(heap.fallback_textures[0].index() == 0 &&
		                     heap.fallback_sampler.index() == 0 &&
		                     heap.fallback_buffer.index() == 0,
		    "the fallbacks must be the first entries of their pools");

		for (u32 i = 0; i < (u32)TextureType::Count; ++i)
		{
			const TextureHot& hot = *b.textures.get(heap.fallback_textures[i]);
			heap.sampled_fallbacks[i] = hot.view;
			heap.storage_fallbacks[storage_array((TextureType)i)] = hot.storage_view;
		}
		heap.sampler_fallback = b.samplers.get(heap.fallback_sampler)->sampler;
		heap.buffer_fallback = b.buffers.get(heap.fallback_buffer)->buffer;
		heap.fallbacks_live = true;

		// Initialize fallback texels to white and buffer bytes to zero. Keep the images in
		// GENERAL so their sampled and storage descriptors can use the same layout.
		const bool filled = boot_commands(b,
		    [&](VkCommandBuffer commands)
		    {
			    for (u32 i = 0; i < (u32)TextureType::Count; ++i)
			    {
				    const TextureHot& hot = *b.textures.get(heap.fallback_textures[i]);
				    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
				        VK_REMAINING_ARRAY_LAYERS};

				    const VkImageMemoryBarrier2 to_general{
				        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
				        .srcAccessMask = VK_ACCESS_2_NONE,
				        .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
				        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
				        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
				        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
				        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				        .image = hot.image,
				        .subresourceRange = whole,
				    };
				    const VkDependencyInfo dependency{
				        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
				        .imageMemoryBarrierCount = 1,
				        .pImageMemoryBarriers = &to_general,
				    };
				    vkCmdPipelineBarrier2(commands, &dependency);

				    const VkClearColorValue white{.float32 = {1.0f, 1.0f, 1.0f, 1.0f}};
				    vkCmdClearColorImage(commands, hot.image, VK_IMAGE_LAYOUT_GENERAL, &white, 1,
				        &whole);
			    }

			    vkCmdFillBuffer(commands, heap.buffer_fallback, 0, VK_WHOLE_SIZE, 0);

			    const VkMemoryBarrier2 done{
			        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			        .srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
			        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			    };
			    const VkDependencyInfo dependency{
			        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			        .memoryBarrierCount = 1,
			        .pMemoryBarriers = &done,
			    };
			    vkCmdPipelineBarrier2(commands, &dependency);
		    });

		if (!filled)
		{
			return false;
		}

		const u32 textures = b.def.limits.max_textures;
		for (u32 i = 0; i < SAMPLED_ARRAY_COUNT; ++i)
		{
			const VkDescriptorImageInfo info{VK_NULL_HANDLE, heap.sampled_fallbacks[i],
			    VK_IMAGE_LAYOUT_GENERAL};
			flood(ctx, heap.set, BINDING_SAMPLED_2D + i, textures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			    &info, nullptr);
		}
		for (u32 i = 0; i < STORAGE_ARRAY_COUNT; ++i)
		{
			const VkDescriptorImageInfo info{VK_NULL_HANDLE, heap.storage_fallbacks[i],
			    VK_IMAGE_LAYOUT_GENERAL};
			flood(ctx, heap.set, BINDING_STORAGE_2D + i, textures, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			    &info, nullptr);
		}

		const VkDescriptorImageInfo sampler_info{heap.sampler_fallback, VK_NULL_HANDLE,
		    VK_IMAGE_LAYOUT_UNDEFINED};
		flood(ctx, heap.set, BINDING_SAMPLERS, b.def.limits.max_samplers,
		    VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_info, nullptr);

		const VkDescriptorBufferInfo buffer_info{heap.buffer_fallback, 0, VK_WHOLE_SIZE};
		flood(ctx, heap.set, BINDING_STORAGE_BUFFERS, b.def.limits.max_buffers,
		    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buffer_info);

		return true;
	}

	void heap_bind_constants(Backend& b)
	{
		for (u32 slot = 0; slot < CONSTANT_SLOTS; ++slot)
		{
			write_buffer(b.ctx, b.heap.constants, slot, 0,
			    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, b.transient.buffer, 0,
			    MAX_CONSTANT_BLOCK_BYTES);
		}
	}

	void heap_release_fallbacks(Backend& b)
	{
		Heap& heap = b.heap;

		// Stop descriptor resets before destroying the fallback views they would write.
		heap.fallbacks_live = false;

		for (TextureHandle& texture : heap.fallback_textures)
		{
			destroy(texture);
			texture = {};
		}

		destroy(heap.fallback_sampler);
		destroy(heap.fallback_buffer);
		heap.fallback_sampler = {};
		heap.fallback_buffer = {};
	}

	void heap_shutdown(Backend& b)
	{
		const Context& ctx = b.ctx;
		Heap& heap = b.heap;

		// The pool owns both sets. Null handles permit cleanup after partial boot failure.
		vkDestroyDescriptorPool(ctx.device, heap.pool, &ctx.callbacks);
		vkDestroyPipelineLayout(ctx.device, heap.pipeline_layout, &ctx.callbacks);
		vkDestroyDescriptorSetLayout(ctx.device, heap.heap_layout, &ctx.callbacks);
		vkDestroyDescriptorSetLayout(ctx.device, heap.constants_layout, &ctx.callbacks);
		heap = Heap{};
	}

	void heap_write_sampled(Backend& b, u32 slot, VkImageView view, VkImageLayout layout,
	    TextureType type)
	{
		write_image(b.ctx, b.heap.set, BINDING_SAMPLED_2D + (u32)type, slot,
		    VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, VK_NULL_HANDLE, layout);
	}

	void heap_write_storage(Backend& b, u32 slot, VkImageView view, TextureType type)
	{
		write_image(b.ctx, b.heap.set, BINDING_STORAGE_2D + storage_array(type), slot,
		    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL);
	}

	void heap_write_sampler(Backend& b, u32 slot, VkSampler sampler)
	{
		write_image(b.ctx, b.heap.set, BINDING_SAMPLERS, slot, VK_DESCRIPTOR_TYPE_SAMPLER,
		    VK_NULL_HANDLE, sampler, VK_IMAGE_LAYOUT_UNDEFINED);
	}

	void heap_write_buffer(Backend& b, u32 slot, VkBuffer buffer, u64 size)
	{
		write_buffer(b.ctx, b.heap.set, BINDING_STORAGE_BUFFERS, slot,
		    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer, 0, size);
	}

	void heap_reset_texture(Backend& b, u32 slot, TextureType type)
	{
		if (!b.heap.fallbacks_live)
		{
			return;
		}

		heap_write_sampled(b, slot, b.heap.sampled_fallbacks[(u32)type], VK_IMAGE_LAYOUT_GENERAL,
		    type);
		heap_write_storage(b, slot, b.heap.storage_fallbacks[storage_array(type)], type);
	}

	void heap_reset_sampler(Backend& b, u32 slot)
	{
		if (b.heap.fallbacks_live)
		{
			heap_write_sampler(b, slot, b.heap.sampler_fallback);
		}
	}

	void heap_reset_buffer(Backend& b, u32 slot)
	{
		if (b.heap.fallbacks_live)
		{
			heap_write_buffer(b, slot, b.heap.buffer_fallback, VK_WHOLE_SIZE);
		}
	}
}
