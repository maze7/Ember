// Each buffer owns a Vulkan buffer and a VMA allocation. Its pool index doubles as its
// descriptor index when shaders use it as storage. All buffers accept transfer writes so
// upload() can fill them. Upload and Readback allocations stay mapped and require coherent
// memory, so CPU access needs no flush or invalidate. Callers still synchronize GPU access.

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

		const VkBufferCreateInfo info{
		    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		    .size = def.size,
		    .usage = to_vk_usage(def.usage),
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		};

		VmaAllocationCreateInfo allocation_info = {};
		switch (def.memory)
		{
			case MemoryLocation::DeviceLocal:
				allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
				break;

			// Prefer device memory the CPU can map, and tell VMA that writes will be sequential.
			// Host visibility and coherence are required; device-local placement is a preference.
			case MemoryLocation::Upload:
				allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
				allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
				                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
				allocation_info.requiredFlags =
				    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				break;

			// Prefer host memory that caches CPU reads. Coherence is required, while caching
			// is a preference, so reading back still works when that memory type is unavailable.
			case MemoryLocation::Readback:
				allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
				allocation_info.flags =
				    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
				allocation_info.requiredFlags =
				    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				allocation_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
				break;

			case MemoryLocation::Count:
				return {};
		}

		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VmaAllocationInfo mapping = {};

		if (const VkResult result = vmaCreateBuffer(b->ctx.allocator, &info, &allocation_info,
		        &buffer, &allocation, &mapping);
		    result != VK_SUCCESS)
		{
			EMBER_ERROR("gpu: buffer '{}' creation failed: {}", def.name, vk::result_name(result));
			return {};
		}

		b->resources_lock.lock();
		const BufferHandle handle = b->buffers.emplace(vk::BufferHot{.buffer = buffer});

		if (!handle.is_null())
		{
			vk::BufferCold& cold = b->buffer_cold[handle.index()];
			cold = vk::BufferCold{};
			cold.allocation = allocation;
			cold.size = def.size;
			cold.mapped = mapping.pMappedData;
			cold.usage = def.usage;
			cold.memory = def.memory;

			// Reclaimed slots have already waited for their old GPU users. Publish the storage
			// descriptor before returning the new handle, while pool changes remain locked.
			if (has_any(def.usage, BufferUsage::Storage))
			{
				vk::heap_write_buffer(*b, handle.index(), buffer, def.size);
			}
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
