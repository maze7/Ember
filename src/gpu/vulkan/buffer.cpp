// Each buffer owns a Vulkan buffer and a VMA allocation. Its pool index doubles as its
// descriptor index when shaders use it as storage. All buffers accept transfer writes so
// upload() can fill them. Upload and Readback allocations stay mapped and require coherent
// memory, so CPU access needs no flush or invalidate. Callers still synchronize GPU access.
// Replacement puts a new buffer behind an existing entry, keeping the handle and its index.

#include <ember/gpu/gpu.h>

#include <gpu/vulkan/backend.h>

namespace ember::gpu
{
	namespace
	{
		VkBufferUsageFlags to_vk_usage(BufferUsage usage)
		{
			VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			if (has_any(usage, BufferUsage::Index))
			{
				flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			}
			if (has_any(usage, BufferUsage::Storage))
			{
				flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			}
			if (has_any(usage, BufferUsage::Indirect))
			{
				flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
			}
			if (has_any(usage, BufferUsage::CopySrc))
			{
				flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			}

			return flags;
		}

		bool same_shape(const vk::BufferCold& cold, const BufferDef& def)
		{
			return cold.size == def.size && cold.usage == def.usage && cold.memory == def.memory;
		}

		// Create the Vulkan buffer and its memory for def, mapped when the location calls for it.
		bool create_native(vk::Backend& b, const BufferDef& def, VkBuffer& buffer,
		    VmaAllocation& allocation, VmaAllocationInfo& mapping)
		{
			const vk::BufferSharing sharing = vk::buffer_sharing(b.ctx);
			const VkBufferCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			    .size = def.size,
			    .usage = to_vk_usage(def.usage),
			    .sharingMode = sharing.mode,
			    .queueFamilyIndexCount = sharing.count,
			    .pQueueFamilyIndices = sharing.families,
			};

			VmaAllocationCreateInfo allocation_info = {};
			switch (def.memory)
			{
				case MemoryLocation::DeviceLocal:
					allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
					break;

				// Prefer device memory the CPU can map, and tell VMA that writes will be
				// sequential. Host visibility and coherence are required; device-local placement
				// is a preference.
				case MemoryLocation::Upload:
					allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
					allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
					                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
					allocation_info.requiredFlags =
					    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
					break;

				// Prefer host memory that caches CPU reads. Coherence is required, while caching
				// is a preference, so reading back still works when that memory type is
				// unavailable.
				case MemoryLocation::Readback:
					allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
					allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
					                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
					allocation_info.requiredFlags =
					    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
					allocation_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
					break;

				case MemoryLocation::Count:
					return false;
			}

			if (const VkResult result = vmaCreateBuffer(b.ctx.allocator, &info, &allocation_info,
			        &buffer, &allocation, &mapping);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: buffer '{}' creation failed: {}", def.name,
				    vk::result_name(result));
				return false;
			}

			return true;
		}

		// Record the def and the native objects in the handle's entry and write its storage
		// descriptor. Reclaimed slots have already waited for their old GPU users. The caller
		// holds resources_lock.
		void publish(vk::Backend& b, BufferHandle handle, const BufferDef& def, VkBuffer buffer,
		    VmaAllocation allocation, const VmaAllocationInfo& mapping)
		{
			b.buffers.get(handle)->buffer = buffer;

			vk::BufferCold& cold = b.buffer_cold[handle.index()];
			cold = vk::BufferCold{};
			cold.allocation = allocation;
			cold.size = def.size;
			cold.mapped = mapping.pMappedData;
			cold.usage = def.usage;
			cold.memory = def.memory;

			if (has_any(def.usage, BufferUsage::Storage))
			{
				vk::heap_write_buffer(b, handle.index(), buffer, def.size);
			}
		}
	}

	namespace vk
	{
		BufferSharing buffer_sharing(const Context& ctx)
		{
			BufferSharing sharing;
			if (ctx.queue_count > 1)
			{
				sharing.mode = VK_SHARING_MODE_CONCURRENT;
				sharing.count = ctx.queue_count;
				for (u32 i = 0; i < ctx.queue_count; ++i)
				{
					sharing.families[i] = ctx.queues[i].family;
				}
			}

			return sharing;
		}
	}

	BufferHandle create_buffer(const BufferDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: buffer '{}' has an invalid def", def.name);
			return {};
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo mapping = {};
		if (!create_native(*b, def, buffer, allocation, mapping))
		{
			return {};
		}

		b->resources_lock.lock();
		const BufferHandle handle = b->buffers.emplace();
		if (!handle.is_null())
		{
			publish(*b, handle, def, buffer, allocation, mapping);
		}
		b->resources_lock.unlock();

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: buffer pool full, '{}' refused", def.name);
			vmaDestroyBuffer(b->ctx.allocator, buffer, allocation);
			return {};
		}

		vk::set_name(b->ctx, VK_OBJECT_TYPE_BUFFER, (u64)buffer, def.name);
		return handle;
	}

	bool replace(BufferHandle handle, const BufferDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return false;
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: buffer '{}' has an invalid def", def.name);
			return false;
		}

		const vk::BufferHot* hot = b->buffers.get(handle);
		if (hot == nullptr)
		{
			EMBER_ERROR("gpu: replace of a stale buffer with '{}'", def.name);
			return false;
		}

		if (handle == b->transient.handle)
		{
			EMBER_ERROR("gpu: '{}' would replace the transient ring", def.name);
			return false;
		}

		const vk::BufferCold& cold = b->buffer_cold[handle.index()];
		if (same_shape(cold, def))
		{
			return true;
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo mapping = {};
		if (!create_native(*b, def, buffer, allocation, mapping))
		{
			return false;
		}

		// A pending frame may be reading this entry's descriptor, which Vulkan forbids rewriting
		// until it has completed. Between frames nothing is open, so waiting out the frames in
		// flight leaves nothing pending.
		EMBER_ASSERT_MSG(!vk::any_frame_open(*b), "replace() with a new shape needs no frame open");
		vk::wait_frames(*b);

		b->resources_lock.lock();
		vk::bury(*b, {.kind = vk::Dead::Kind::Buffer,
		                 .handle = (u64)hot->buffer,
		                 .allocation = cold.allocation});
		vk::heap_reset_buffer(*b, handle.index());
		publish(*b, handle, def, buffer, allocation, mapping);
		b->resources_lock.unlock();

		vk::set_name(b->ctx, VK_OBJECT_TYPE_BUFFER, (u64)buffer, def.name);
		return true;
	}

	void destroy(BufferHandle handle)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		b->resources_lock.lock();

		const vk::BufferHot* hot = b->buffers.get(handle);
		if (hot == nullptr)
		{
			b->resources_lock.unlock();
			return;
		}

		const vk::BufferCold& cold = b->buffer_cold[handle.index()];
		vk::bury(*b, {.kind = vk::Dead::Kind::Buffer,
		                 .handle = (u64)hot->buffer,
		                 .allocation = cold.allocation});

		// Invalidate the handle now, but leave its descriptor index unavailable until the
		// graveyard releases it. Submitted shaders only have the index, without a generation.
		(void)b->buffers.retire(handle);
		vk::bury(*b, {.kind = vk::Dead::Kind::BufferSlot, .handle = handle.index()});

		b->resources_lock.unlock();
	}

	bool is_valid(BufferHandle handle)
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->buffers.get(handle) != nullptr;
	}

	void* mapped(BufferHandle buffer)
	{
		const vk::Backend* b = vk::backend();
		if (b == nullptr || b->buffers.get(buffer) == nullptr)
		{
			return nullptr;
		}

		return b->buffer_cold[buffer.index()].mapped;
	}
}
