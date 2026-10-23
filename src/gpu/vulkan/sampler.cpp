// A sampler's pool index is its descriptor index. Keep both the Vulkan object and that slot
// until the graveyard has finished with the frames that could still sample through them.
// Replacement puts a new sampler behind an existing entry, keeping the handle and its index.

#include <ember/gpu/gpu.h>

#include <ember/math/scalar.h>
#include <gpu/vulkan/backend.h>

namespace ember::gpu
{
	namespace
	{
		VkFilter to_vk(Filter filter)
		{
			return filter == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
		}

		VkSamplerAddressMode to_vk(AddressMode mode)
		{
			switch (mode)
			{
				case AddressMode::Repeat:
					return VK_SAMPLER_ADDRESS_MODE_REPEAT;
				case AddressMode::MirroredRepeat:
					return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
				case AddressMode::ClampToEdge:
					return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				case AddressMode::ClampToBorder:
					return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
				case AddressMode::Count:
					break;
			}

			return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		}

		VkBorderColor to_vk(BorderColor color)
		{
			switch (color)
			{
				case BorderColor::TransparentBlack:
					return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
				case BorderColor::OpaqueBlack:
					return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
				case BorderColor::OpaqueWhite:
					return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
				case BorderColor::Count:
					break;
			}

			return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		}

		// Build the Vulkan sampler for def, or null with the failure logged.
		VkSampler create_native(vk::Backend& b, const SamplerDef& def)
		{
			const Caps& caps = b.ctx.caps;

			// Min and max reduction are optional. When unsupported, leave the extension off the
			// creation chain so the sampler uses weighted averaging, and report the fallback.
			VkSamplerReductionModeCreateInfo reduction{
			    .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
			    .reductionMode = def.reduction == ReductionMode::Min
			                         ? VK_SAMPLER_REDUCTION_MODE_MIN
			                         : VK_SAMPLER_REDUCTION_MODE_MAX,
			};

			const void* next = nullptr;
			if (def.reduction != ReductionMode::WeightedAverage)
			{
				if (caps.sampler_minmax)
				{
					next = &reduction;
				}
				else
				{
					EMBER_WARN("gpu: sampler '{}' asked for a reduction mode the adapter lacks",
					    def.name);
				}
			}

			const u32 anisotropy = min((u32)def.max_anisotropy, caps.max_anisotropy);

			const VkSamplerCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			    .pNext = next,
			    .magFilter = to_vk(def.mag_filter),
			    .minFilter = to_vk(def.min_filter),
			    .mipmapMode = def.mip_filter == Filter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
			                                                    : VK_SAMPLER_MIPMAP_MODE_LINEAR,
			    .addressModeU = to_vk(def.address_u),
			    .addressModeV = to_vk(def.address_v),
			    .addressModeW = to_vk(def.address_w),
			    .mipLodBias = def.mip_lod_bias,
			    .anisotropyEnable = anisotropy > 1 ? VK_TRUE : VK_FALSE,
			    .maxAnisotropy = (f32)max(anisotropy, 1u),
			    .compareEnable = def.compare_enable ? VK_TRUE : VK_FALSE,
			    .compareOp = vk::to_vk(def.compare),
			    .minLod = def.min_lod,
			    .maxLod = def.max_lod >= LOD_NONE ? VK_LOD_CLAMP_NONE : def.max_lod,
			    .borderColor = to_vk(def.border),
			    .unnormalizedCoordinates = VK_FALSE,
			};

			VkSampler sampler = VK_NULL_HANDLE;
			if (const VkResult result =
			        vkCreateSampler(b.ctx.device, &info, &b.ctx.callbacks, &sampler);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: sampler '{}' creation failed: {}", def.name,
				    vk::result_name(result));
				return VK_NULL_HANDLE;
			}

			return sampler;
		}
	}

	SamplerHandle create_sampler(const SamplerDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: sampler '{}' has an invalid def", def.name);
			return {};
		}

		const VkSampler sampler = create_native(*b, def);
		if (sampler == VK_NULL_HANDLE)
		{
			return {};
		}

		b->resources_lock.lock();
		const SamplerHandle handle = b->samplers.emplace(vk::SamplerData{.sampler = sampler});
		if (!handle.is_null())
		{
			vk::heap_write_sampler(*b, handle.index(), sampler);
		}
		b->resources_lock.unlock();

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: sampler pool full, '{}' refused", def.name);
			vkDestroySampler(b->ctx.device, sampler, &b->ctx.callbacks);
			return {};
		}

		vk::set_name(b->ctx, VK_OBJECT_TYPE_SAMPLER, (u64)sampler, def.name);
		return handle;
	}

	bool replace(SamplerHandle handle, const SamplerDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return false;
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: sampler '{}' has an invalid def", def.name);
			return false;
		}

		vk::SamplerData* data = b->samplers.get(handle);
		if (data == nullptr)
		{
			EMBER_ERROR("gpu: replace of a stale sampler with '{}'", def.name);
			return false;
		}

		const VkSampler sampler = create_native(*b, def);
		if (sampler == VK_NULL_HANDLE)
		{
			return false;
		}

		// A pending frame may be sampling through this entry's descriptor, which Vulkan forbids
		// rewriting until it has completed. Between frames nothing is open, so waiting out the
		// frames in flight leaves nothing pending.
		EMBER_ASSERT_MSG(!vk::any_frame_open(*b), "replace() of a sampler needs no frame open");
		vk::wait_frames(*b);

		b->resources_lock.lock();
		vk::bury(*b, {.kind = vk::Dead::Kind::Sampler, .handle = (u64)data->sampler});
		data->sampler = sampler;
		vk::heap_write_sampler(*b, handle.index(), sampler);
		b->resources_lock.unlock();

		vk::set_name(b->ctx, VK_OBJECT_TYPE_SAMPLER, (u64)sampler, def.name);
		return true;
	}

	void destroy(SamplerHandle handle)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		b->resources_lock.lock();

		if (const vk::SamplerData* data = b->samplers.get(handle))
		{
			vk::bury(*b, {.kind = vk::Dead::Kind::Sampler, .handle = (u64)data->sampler});
			(void)b->samplers.retire(handle);
			vk::bury(*b, {.kind = vk::Dead::Kind::SamplerSlot, .handle = handle.index()});
		}

		b->resources_lock.unlock();
	}

	bool is_valid(SamplerHandle handle)
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->samplers.get(handle) != nullptr;
	}
}
