// Transient and staging buffers are divided by frame slot and reset after GPU completion.
// Workers reserve chunks, then use private cursors; external threads reserve directly.
// The transient buffer is shader-visible. Staging stays private and retains CPU copies until
// their upload batches run on the graphics queue.

#include <ember/gpu/gpu.h>
#include <ember/math/scalar.h>

#include <ember/core/jobs.h>
#include <ember/memory/alignment.h>
#include <ember/memory/heap.h>
#include <gpu/vulkan/backend.h>

#include <cstring>
#include <new>

namespace ember::gpu
{
	namespace
	{
		// Chunk refills amortize shared-counter contention but charge unused tails to the frame.
		// Bases align to 256 to satisfy every supported power-of-two alignment.
		constexpr u64 CHUNK_BYTES = 64_kb;
		constexpr u64 CHUNK_ALIGNMENT = 256;

		// Sixteen is a multiple of every format's texel block size and the buffer-copy alignment.
		constexpr u64 STAGING_ALIGNMENT = 16;

		void init_slices(vk::Backend& b, vk::Ring& ring)
		{
			ring.worker_count = jobs::worker_count();
			ring.slices = (vk::RingSlice*)memory::allocate(memory::Tag::Graphics,
			    sizeof(vk::RingSlice) * b.ctx.in_flight, alignof(vk::RingSlice));

			for (u32 slot = 0; slot < b.ctx.in_flight; ++slot)
			{
				vk::RingSlice& slice = *::new ((void*)(ring.slices + slot)) vk::RingSlice();
				slice.cursors = (vk::RingCursor*)memory::allocate_zeroed(memory::Tag::Graphics,
				    sizeof(vk::RingCursor) * ring.worker_count, alignof(vk::RingCursor));
				vk::ring_reset(ring, slot);
			}
		}

		// Require a persistent, coherent mapping so staging writes need no explicit flush.
		// The frame slot controls when the CPU can reuse bytes the GPU may still read.
		bool create_ring(vk::Backend& b, vk::Ring& ring, const char* name, u64 slice_bytes,
		    VkBufferUsageFlags usage)
		{
			const u64 bytes = slice_bytes * b.ctx.in_flight;
			if (bytes > 0xffffffffu)
			{
				EMBER_ERROR("gpu: the {} ring would be {} bytes; offsets are 32 bit", name, bytes);
				return false;
			}

			const vk::BufferSharing sharing = vk::buffer_sharing(b.ctx);
			const VkBufferCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			    .size = bytes,
			    .usage = usage,
			    .sharingMode = sharing.mode,
			    .queueFamilyIndexCount = sharing.count,
			    .pQueueFamilyIndices = sharing.families,
			};

			VmaAllocationCreateInfo allocation_info = {};
			allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
			allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
			allocation_info.requiredFlags =
			    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

			VmaAllocationInfo mapping = {};
			if (const VkResult result = vmaCreateBuffer(b.ctx.allocator, &info, &allocation_info,
			        &ring.buffer, &ring.allocation, &mapping);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: the {} ring failed: {}", name, vk::result_name(result));
				return false;
			}

			ring.cpu = (u8*)mapping.pMappedData;
			ring.slice_bytes = slice_bytes;
			ring.name = name;
			init_slices(b, ring);

			vk::set_name(b.ctx, VK_OBJECT_TYPE_BUFFER, (u64)ring.buffer, name);
			return true;
		}

		void destroy_ring(vk::Backend& b, vk::Ring& ring)
		{
			if (ring.slices != nullptr)
			{
				for (u32 slot = 0; slot < b.ctx.in_flight; ++slot)
				{
					memory::free(ring.slices[slot].cursors);
					ring.slices[slot].~RingSlice();
				}
				memory::free(ring.slices);
			}

			// The transient ring's pool entry owns its storage; only staging releases it here.
			if (ring.handle.is_null())
			{
				vmaDestroyBuffer(b.ctx.allocator, ring.buffer, ring.allocation);
			}

			ring = vk::Ring{};
		}

		// Hold upload_lock. The first barrier orders writes after earlier graphics accesses;
		// independent queues need explicit dependencies.
		VkCommandBuffer open_upload_batch(vk::Slot& slot)
		{
			if (slot.upload_open)
			{
				return slot.upload_batches[slot.upload_batch];
			}

			if (slot.upload_batch >= vk::MAX_UPLOAD_BATCHES)
			{
				return VK_NULL_HANDLE;
			}

			const VkCommandBuffer commands = slot.upload_batches[slot.upload_batch];
			const VkCommandBufferBeginInfo begin{
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			};
			EMBER_VK_CHECK(vkBeginCommandBuffer(commands, &begin));

			const VkMemoryBarrier2 before{
			    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
			    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			};
			const VkDependencyInfo dependency{
			    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			    .memoryBarrierCount = 1,
			    .pMemoryBarriers = &before,
			};
			vkCmdPipelineBarrier2(commands, &dependency);

			slot.upload_open = true;
			return commands;
		}
	}

	namespace vk
	{
		bool rings_boot(Backend& b)
		{
			Ring& ring = b.transient;

			// Each dynamic constant descriptor exposes a fixed-size window. Pad the last slice
			// so that window stays inside the buffer even when its offset is near the end.
			const u64 bytes = b.def.transient_bytes * b.ctx.in_flight + MAX_CONSTANT_BLOCK_BYTES;
			if (bytes > 0xffffffffu)
			{
				EMBER_ERROR("gpu: the transient ring would be {} bytes; offsets are 32 bit", bytes);
				return false;
			}

			// Constants require uniform-buffer usage, which public BufferDef does not expose.
			const BufferSharing sharing = buffer_sharing(b.ctx);
			const VkBufferCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			    .size = bytes,
			    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
			             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    .sharingMode = sharing.mode,
			    .queueFamilyIndexCount = sharing.count,
			    .pQueueFamilyIndices = sharing.families,
			};

			// Coherent mapping is required; device-local placement is only preferred.
			VmaAllocationCreateInfo allocation_info = {};
			allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
			allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
			allocation_info.requiredFlags =
			    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

			VmaAllocationInfo mapping = {};
			if (const VkResult result = vmaCreateBuffer(b.ctx.allocator, &info, &allocation_info,
			        &ring.buffer, &ring.allocation, &mapping);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: the transient ring failed: {}", vk::result_name(result));
				return false;
			}

			// A pool handle exposes transient allocations through the shader buffer interface.
			b.resources_lock.lock();
			ring.handle = b.buffers.emplace(BufferHot{.buffer = ring.buffer});
			if (!ring.handle.is_null())
			{
				BufferCold& cold = b.buffer_cold[ring.handle.index()];
				cold = BufferCold{};
				cold.allocation = ring.allocation;
				cold.size = bytes;
				cold.mapped = mapping.pMappedData;
				cold.usage = BufferUsage::Storage | BufferUsage::Index | BufferUsage::Indirect |
				             BufferUsage::CopySrc;
				cold.memory = MemoryLocation::Upload;
				heap_write_buffer(b, ring.handle.index(), ring.buffer, bytes);
			}
			b.resources_lock.unlock();

			if (ring.handle.is_null())
			{
				EMBER_ERROR("gpu: no pool slot for the transient ring");
				vmaDestroyBuffer(b.ctx.allocator, ring.buffer, ring.allocation);
				ring = Ring{};
				return false;
			}

			ring.cpu = (u8*)mapping.pMappedData;
			ring.slice_bytes = b.def.transient_bytes;
			ring.name = "ember.transient";
			init_slices(b, ring);
			set_name(b.ctx, VK_OBJECT_TYPE_BUFFER, (u64)ring.buffer, ring.name);

			return create_ring(b, b.staging, "ember.staging", b.def.staging_bytes,
			    VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		}

		void rings_shutdown(Backend& b)
		{
			destroy_ring(b, b.staging);

			const BufferHandle transient = b.transient.handle;
			destroy_ring(b, b.transient);
			destroy(transient);
		}

		void ring_reset(Ring& ring, u32 slot)
		{
			RingSlice& slice = ring.slices[slot];
			slice.next.store(slot * ring.slice_bytes, std::memory_order_relaxed);
			slice.starved.store(false, std::memory_order_relaxed);
			for (u32 i = 0; i < ring.worker_count; ++i)
			{
				slice.cursors[i] = {};
			}
		}

		RingSpan ring_allocate(Ring& ring, u32 slot, u64 size, u64 alignment)
		{
			EMBER_ASSERT(alignment >= 1 && alignment <= CHUNK_ALIGNMENT);

			RingSlice& slice = ring.slices[slot];
			const u64 slice_end = (slot + 1) * ring.slice_bytes;

			// Report the first reservation that runs past the slice. Failed reservations remain
			// charged until reset; workers may still use space left in chunks they already hold.
			const auto starve = [&]() -> RingSpan
			{
				if (!slice.starved.exchange(true, std::memory_order_relaxed))
				{
					EMBER_ERROR("gpu: the {} ring is spent for this frame; raise GpuDef's budget",
					    ring.name);
				}
				return {};
			};

			// Use the current worker's private cursor until it needs another chunk. This call
			// never waits, so a fiber cannot move to another worker while that cursor is in use.
			const u32 worker = jobs::worker_index();
			if (worker != jobs::NO_WORKER && worker < ring.worker_count)
			{
				RingCursor& cursor = slice.cursors[worker];
				u64 start = align_up(cursor.cursor, alignment);

				if (start + size > cursor.end)
				{
					const u64 wanted = align_up(size, CHUNK_ALIGNMENT);
					const u64 chunk = wanted > CHUNK_BYTES ? wanted : CHUNK_BYTES;
					const u64 base = slice.next.fetch_add(chunk, std::memory_order_relaxed);

					if (base + chunk > slice_end)
					{
						return starve();
					}

					cursor.cursor = base;
					cursor.end = base + chunk;
					start = base;
				}

				cursor.cursor = start + size;
				return {ring.cpu + start, start};
			}

			// External threads have no private cursor, so reserve only this request plus alignment.
			const u64 chunk = align_up(size, CHUNK_ALIGNMENT);
			const u64 base = slice.next.fetch_add(chunk, std::memory_order_relaxed);
			if (base + chunk > slice_end)
			{
				return starve();
			}

			return {ring.cpu + base, base};
		}

		u64 ring_used(const Ring& ring, u32 slot)
		{
			const u64 next = ring.slices[slot].next.load(std::memory_order_relaxed);
			const u64 begin = slot * ring.slice_bytes;
			const u64 end = begin + ring.slice_bytes;
			return min(next, end) - begin;
		}

		VkCommandBuffer flush_uploads(Slot& slot)
		{
			slot.upload_lock.lock();

			if (!slot.upload_open)
			{
				slot.upload_lock.unlock();
				return VK_NULL_HANDLE;
			}

			// Make this batch's transfer writes visible to later accesses on the graphics queue.
			// Submitting it does not synchronize independent queues on its own.
			const VkCommandBuffer commands = slot.upload_batches[slot.upload_batch];
			const VkMemoryBarrier2 after{
			    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
			    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
			};
			const VkDependencyInfo dependency{
			    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			    .memoryBarrierCount = 1,
			    .pMemoryBarriers = &after,
			};
			vkCmdPipelineBarrier2(commands, &dependency);
			EMBER_VK_CHECK(vkEndCommandBuffer(commands));

			slot.upload_open = false;
			++slot.upload_batch;

			slot.upload_lock.unlock();
			return commands;
		}
	}

	Transient allocate_transient(const Frame& frame, u32 size, u32 alignment)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr || size == 0)
		{
			return {};
		}

		vk::Slot& slot = vk::slot_of(*b, frame);
		EMBER_ASSERT_MSG(vk::is_open(slot), "transient memory for a frame that has ended");

		const vk::RingSpan span = vk::ring_allocate(b->transient, frame.slot, size, alignment);
		if (span.cpu == nullptr)
		{
			return {};
		}

		return {
		    .cpu = span.cpu,
		    .buffer = b->transient.handle,
		    .offset = (u32)span.offset,
		    .size = size,
		};
	}

	bool upload(const Frame& frame, BufferHandle buffer, u64 offset, Span<const u8> data)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr || data.empty())
		{
			return b != nullptr;
		}

		const vk::BufferHot* hot = b->buffers.get(buffer);
		if (hot == nullptr)
		{
			EMBER_ERROR("gpu: upload to a stale buffer");
			return false;
		}

		const vk::BufferCold& cold = b->buffer_cold[buffer.index()];
		if (offset + data.size() > cold.size)
		{
			EMBER_ERROR("gpu: upload of {} bytes at {} overruns a {} byte buffer", data.size(),
			    offset, cold.size);
			return false;
		}

		vk::Slot& slot = vk::slot_of(*b, frame);
		EMBER_ASSERT_MSG(vk::is_open(slot), "an upload for a frame that has ended");

		const vk::RingSpan stage =
		    vk::ring_allocate(b->staging, frame.slot, data.size(), STAGING_ALIGNMENT);
		if (stage.cpu == nullptr)
		{
			return false;
		}

		std::memcpy(stage.cpu, data.data(), data.size());

		slot.upload_lock.lock();
		const VkCommandBuffer commands = open_upload_batch(slot);
		if (commands == VK_NULL_HANDLE)
		{
			slot.upload_lock.unlock();
			EMBER_ERROR("gpu: frame {} flushed {} upload batches already; the upload is refused",
			    frame.index, vk::MAX_UPLOAD_BATCHES);
			return false;
		}

		const VkBufferCopy2 region{
		    .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
		    .srcOffset = stage.offset,
		    .dstOffset = offset,
		    .size = data.size(),
		};
		const VkCopyBufferInfo2 copy{
		    .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
		    .srcBuffer = b->staging.buffer,
		    .dstBuffer = hot->buffer,
		    .regionCount = 1,
		    .pRegions = &region,
		};
		vkCmdCopyBuffer2(commands, &copy);
		slot.upload_lock.unlock();

		return true;
	}

	bool upload(const Frame& frame, TextureHandle texture, u32 mip, u32 layer, Span<const u8> data)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr || data.empty())
		{
			return b != nullptr;
		}

		const vk::TextureHot* hot = b->textures.get(texture);
		if (hot == nullptr)
		{
			EMBER_ERROR("gpu: upload to a stale texture");
			return false;
		}

		const vk::TextureCold& cold = b->texture_cold[texture.index()];
		if (!cold.swapchain.is_null() || !cold.parent.is_null())
		{
			EMBER_ERROR("gpu: upload to a backbuffer or a hidden mip entry");
			return false;
		}

		if (mip >= cold.mip_count || layer >= cold.layer_count)
		{
			EMBER_ERROR("gpu: upload to mip {} layer {} of a texture with {} and {}", mip, layer,
			    cold.mip_count, cold.layer_count);
			return false;
		}

		const Extent3D extent{cold.extent.width, cold.extent.height, cold.extent.depth};
		const u64 expected = subresource_bytes(cold.api_format, extent, mip);
		if (data.size() != expected)
		{
			EMBER_ERROR("gpu: upload of {} bytes to a subresource of {}", data.size(), expected);
			return false;
		}

		vk::Slot& slot = vk::slot_of(*b, frame);
		EMBER_ASSERT_MSG(vk::is_open(slot), "an upload for a frame that has ended");

		EMBER_ASSERT(STAGING_ALIGNMENT % format_info(cold.api_format).block_bytes == 0);
		const vk::RingSpan stage =
		    vk::ring_allocate(b->staging, frame.slot, data.size(), STAGING_ALIGNMENT);
		if (stage.cpu == nullptr)
		{
			return false;
		}

		std::memcpy(stage.cpu, data.data(), data.size());

		slot.upload_lock.lock();
		const VkCommandBuffer commands = open_upload_batch(slot);
		if (commands == VK_NULL_HANDLE)
		{
			slot.upload_lock.unlock();
			EMBER_ERROR("gpu: frame {} flushed {} upload batches already; the upload is refused",
			    frame.index, vk::MAX_UPLOAD_BATCHES);
			return false;
		}

		// Whole-subresource writes discard old contents and finish in the usage's expected layout.
		// These barriers cover graphics only; order prior uses on other queues separately.
		const VkImageSubresourceRange range{vk::aspect_of(cold.api_format), mip, 1, layer, 1};
		const VkImageMemoryBarrier2 to_copy{
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		    .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		    .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
		    .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = hot->image,
		    .subresourceRange = range,
		};
		VkDependencyInfo dependency{
		    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		    .imageMemoryBarrierCount = 1,
		    .pImageMemoryBarriers = &to_copy,
		};
		vkCmdPipelineBarrier2(commands, &dependency);

		const Extent3D size = mip_extent(extent, mip);
		const VkBufferImageCopy2 region{
		    .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
		    .bufferOffset = stage.offset,
		    .imageSubresource = {vk::aspect_of(cold.api_format), mip, layer, 1},
		    .imageExtent = {size.width, size.height, size.depth},
		};
		const VkCopyBufferToImageInfo2 copy{
		    .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
		    .srcBuffer = b->staging.buffer,
		    .dstImage = hot->image,
		    .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .regionCount = 1,
		    .pRegions = &region,
		};
		vkCmdCopyBufferToImage2(commands, &copy);

		const VkImageMemoryBarrier2 to_read{
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		    .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .newLayout = vk::layout_of(cold, vk::uploaded_state(cold.usage)),
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = hot->image,
		    .subresourceRange = range,
		};
		dependency.pImageMemoryBarriers = &to_read;
		vkCmdPipelineBarrier2(commands, &dependency);
		slot.upload_lock.unlock();

		return true;
	}
}
