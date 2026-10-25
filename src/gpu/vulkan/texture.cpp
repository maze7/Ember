// Textures own their image, VMA allocation, views and descriptor slots. Create native objects
// before taking resources_lock, then publish entries and descriptors together. Retirement
// retains storage and indices for pending frames. Replacement keeps the handle and retained
// mip indices while retiring the old storage.

#include <ember/gpu/gpu.h>

#include <ember/memory/heap.h>
#include <gpu/vulkan/backend.h>

namespace ember::gpu
{
	namespace
	{
		// Uploads use transfer writes, so include that usage even when the caller omits CopyDst.
		VkImageUsageFlags to_vk_usage(TextureUsage usage)
		{
			VkImageUsageFlags flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

			if (has_any(usage, TextureUsage::Sampled))
			{
				flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
			}
			if (has_any(usage, TextureUsage::Storage))
			{
				flags |= VK_IMAGE_USAGE_STORAGE_BIT;
			}
			if (has_any(usage, TextureUsage::ColorTarget))
			{
				flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			}
			if (has_any(usage, TextureUsage::DepthTarget))
			{
				flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}
			if (has_any(usage, TextureUsage::CopySrc))
			{
				flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			}
			if (has_any(usage, TextureUsage::CopyDst))
			{
				flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			}

			return flags;
		}

		VkFormatFeatureFlags required_features(TextureUsage usage)
		{
			VkFormatFeatureFlags features = 0;

			if (has_any(usage, TextureUsage::Sampled))
			{
				features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			}
			if (has_any(usage, TextureUsage::Storage))
			{
				features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			}
			if (has_any(usage, TextureUsage::ColorTarget))
			{
				features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			}
			if (has_any(usage, TextureUsage::DepthTarget))
			{
				features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
			}

			return features;
		}

		VkImageViewType to_vk_view_type(TextureType type)
		{
			switch (type)
			{
				case TextureType::Texture2D:
					return VK_IMAGE_VIEW_TYPE_2D;
				case TextureType::Texture2DArray:
					return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
				case TextureType::TextureCube:
					return VK_IMAGE_VIEW_TYPE_CUBE;
				case TextureType::Texture3D:
					return VK_IMAGE_VIEW_TYPE_3D;
				case TextureType::Count:
					break;
			}

			return VK_IMAGE_VIEW_TYPE_2D;
		}

		// Expose a cube's faces as a 2D array for storage access, matching the shader interface.
		VkImageViewType storage_view_type(TextureType type)
		{
			return type == TextureType::TextureCube ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
			                                        : to_vk_view_type(type);
		}

		// Unpublished objects can be released on failure without waiting for the GPU.
		struct Natives
		{
			VkImage image = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageView storage_view = VK_NULL_HANDLE;
			VkImageView mip_views[MAX_MIP_LEVELS] = {};
			VkImageView* attachment_views = nullptr;
			u32 attachment_view_count = 0;
		};

		void destroy_natives(vk::Backend& b, Natives& natives)
		{
			const VkAllocationCallbacks* callbacks = &b.ctx.callbacks;

			for (u32 i = 0; i < natives.attachment_view_count; ++i)
			{
				vkDestroyImageView(b.ctx.device, natives.attachment_views[i], callbacks);
			}
			memory::free(natives.attachment_views);

			for (VkImageView view : natives.mip_views)
			{
				vkDestroyImageView(b.ctx.device, view, callbacks);
			}

			vkDestroyImageView(b.ctx.device, natives.storage_view, callbacks);
			vkDestroyImageView(b.ctx.device, natives.view, callbacks);
			vmaDestroyImage(b.ctx.allocator, natives.image, natives.allocation);
			natives = {};
		}

		bool create_natives(vk::Backend& b, const TextureDef& def, Natives& out)
		{
			const VkFormat format = vk::to_vk(def.format);

			// Reject unsupported format/usage combinations before allocating.
			VkFormatProperties properties = {};
			vkGetPhysicalDeviceFormatProperties(b.ctx.adapter, format, &properties);

			const VkFormatFeatureFlags needed = required_features(def.usage);
			if ((properties.optimalTilingFeatures & needed) != needed)
			{
				EMBER_ERROR("gpu: texture '{}': the adapter lacks its format for its usage",
				    def.name);
				return false;
			}

			const VkImageCreateInfo image_info{
			    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			    .flags = def.type == TextureType::TextureCube
				             ? (VkImageCreateFlags)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
				             : (VkImageCreateFlags)0,
			    .imageType =
			        def.type == TextureType::Texture3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
			    .format = format,
			    .extent = {def.extent.width, def.extent.height, def.extent.depth},
			    .mipLevels = def.mip_count,
			    .arrayLayers = def.layers,
			    .samples = VK_SAMPLE_COUNT_1_BIT,
			    .tiling = VK_IMAGE_TILING_OPTIMAL,
			    .usage = to_vk_usage(def.usage),
			    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};

			VmaAllocationCreateInfo allocation_info = {};
			allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

			if (const VkResult result = vmaCreateImage(b.ctx.allocator, &image_info,
			        &allocation_info, &out.image, &out.allocation, nullptr);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: texture '{}': image creation failed: {}", def.name,
				    vk::result_name(result));
				return false;
			}

			VkImageViewCreateInfo view_info{
			    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			    .image = out.image,
			    .viewType = to_vk_view_type(def.type),
			    .format = format,
			    .subresourceRange = {vk::aspect_of(def.format), 0, def.mip_count, 0, def.layers},
			};

			// The full-chain view serves sampling and single-subresource attachments. A
			// storage-only texture reads and writes through its per-mip views instead.
			if (has_any(def.usage, TextureUsage::Sampled | TextureUsage::ColorTarget |
			                           TextureUsage::DepthTarget) &&
			    vkCreateImageView(b.ctx.device, &view_info, &b.ctx.callbacks, &out.view) !=
			        VK_SUCCESS)
			{
				EMBER_ERROR("gpu: texture '{}': view creation failed", def.name);
				return false;
			}

			// Storage descriptors select one mip. Mip zero uses the texture's own slot; each
			// remaining mip gets a hidden pool entry and descriptor index.
			if (has_any(def.usage, TextureUsage::Storage))
			{
				for (u32 mip = 0; mip < def.mip_count; ++mip)
				{
					VkImageViewCreateInfo mip_info = view_info;
					mip_info.viewType = storage_view_type(def.type);
					mip_info.subresourceRange.baseMipLevel = mip;
					mip_info.subresourceRange.levelCount = 1;

					VkImageView* target = mip == 0 ? &out.storage_view : &out.mip_views[mip];
					if (vkCreateImageView(b.ctx.device, &mip_info, &b.ctx.callbacks, target) !=
					    VK_SUCCESS)
					{
						EMBER_ERROR("gpu: texture '{}': storage view creation failed", def.name);
						return false;
					}
				}
			}

			// Rendering selects one mip and layer at a time. Prebuild those attachment views
			// when the full image view cannot name just that subresource.
			const bool target =
			    has_any(def.usage, TextureUsage::ColorTarget | TextureUsage::DepthTarget);
			if (target && (def.mip_count > 1 || def.layers > 1))
			{
				const u32 count = def.mip_count * def.layers;
				out.attachment_views = (VkImageView*)memory::allocate_zeroed(memory::Tag::Graphics,
				    count * sizeof(VkImageView), alignof(VkImageView));
				out.attachment_view_count = count;

				for (u32 mip = 0; mip < def.mip_count; ++mip)
				{
					for (u32 layer = 0; layer < def.layers; ++layer)
					{
						VkImageViewCreateInfo slice_info = view_info;
						slice_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
						slice_info.subresourceRange = {vk::aspect_of(def.format), mip, 1, layer, 1};

						if (vkCreateImageView(b.ctx.device, &slice_info, &b.ctx.callbacks,
						        &out.attachment_views[mip * def.layers + layer]) != VK_SUCCESS)
						{
							EMBER_ERROR("gpu: texture '{}': attachment view creation failed",
							    def.name);
							return false;
						}
					}
				}
			}

			return true;
		}

		// A name change alone does not require new storage.
		bool same_shape(const vk::TextureCold& cold, const TextureDef& def)
		{
			return cold.type == def.type && cold.api_format == def.format &&
			       cold.extent.width == def.extent.width &&
			       cold.extent.height == def.extent.height &&
			       cold.extent.depth == def.extent.depth && cold.layer_count == def.layers &&
			       cold.mip_count == def.mip_count && cold.usage == def.usage;
		}

		// Hold resources_lock. Reuse existing mip entries to preserve their indices. Pool
		// exhaustion rolls back all new claims and leaves existing entries intact.
		bool claim_mips(vk::Backend& b, const TextureDef& def, const Natives& natives,
		    const vk::TextureCold* reuse, TextureHandle* mips)
		{
			bool full = false;
			for (u32 mip = 1; mip < def.mip_count && natives.mip_views[mip] != VK_NULL_HANDLE;
			    ++mip)
			{
				if (reuse != nullptr && !reuse->mip_storage[mip].is_null())
				{
					mips[mip] = reuse->mip_storage[mip];
					continue;
				}

				mips[mip] = b.textures.emplace();
				full |= mips[mip].is_null();
			}

			if (full)
			{
				for (u32 mip = 1; mip < MAX_MIP_LEVELS; ++mip)
				{
					if (reuse == nullptr || mips[mip] != reuse->mip_storage[mip])
					{
						(void)b.textures.erase(mips[mip]);
					}
					mips[mip] = {};
				}
			}

			return !full;
		}

		// Hold resources_lock. Main and hidden mip slots must have no pending GPU users before
		// their descriptors are written.
		void publish(vk::Backend& b, TextureHandle handle, const TextureDef& def,
		    const Natives& natives, const TextureHandle* mips)
		{
			*b.textures.get(handle) = vk::TextureHot{.image = natives.image,
			    .view = natives.view,
			    .storage_view = natives.storage_view};

			vk::TextureCold& cold = b.texture_cold[handle.index()];
			cold = vk::TextureCold{};
			cold.allocation = natives.allocation;
			cold.extent = {def.extent.width, def.extent.height, def.extent.depth};
			cold.api_format = def.format;
			cold.type = def.type;
			cold.usage = def.usage;
			cold.mip_count = def.mip_count;
			cold.layer_count = def.layers;
			cold.attachment_views = natives.attachment_views;
			cold.attachment_view_count = natives.attachment_view_count;

			// Writing a descriptor does not transition the image into its declared layout.
			if (has_any(def.usage, TextureUsage::Sampled))
			{
				vk::heap_write_sampled(b, handle.index(), natives.view,
				    vk::read_layout_of(def.usage), def.type);
			}
			if (has_any(def.usage, TextureUsage::Storage))
			{
				vk::heap_write_storage(b, handle.index(), natives.storage_view, def.type);
			}

			for (u32 mip = 1; mip < def.mip_count; ++mip)
			{
				cold.mip_storage[mip] = mips[mip];
				if (!mips[mip].is_null())
				{
					*b.textures.get(mips[mip]) = vk::TextureHot{.image = natives.image,
					    .storage_view = natives.mip_views[mip]};

					vk::TextureCold& entry = b.texture_cold[mips[mip].index()];
					entry = vk::TextureCold{};
					entry.extent = cold.extent;
					entry.api_format = def.format;
					entry.type = def.type;
					entry.usage = TextureUsage::Storage;
					entry.parent = handle;
					vk::heap_write_storage(b, mips[mip].index(), natives.mip_views[mip], def.type);
				}
			}
		}

		// Hold resources_lock. Retire native objects but leave the main entry to the caller.
		// Hidden entries below kept_mips retain their indices for replacement; all views retire.
		void retire_natives(vk::Backend& b, const vk::TextureHot& hot, const vk::TextureCold& cold,
		    u32 kept_mips)
		{
			for (u32 mip = 1; mip < MAX_MIP_LEVELS; ++mip)
			{
				const TextureHandle entry = cold.mip_storage[mip];
				if (const vk::TextureHot* hidden = b.textures.get(entry))
				{
					vk::bury(b,
					    {.kind = vk::Dead::Kind::ImageView, .handle = (u64)hidden->storage_view});
					if (mip >= kept_mips)
					{
						(void)b.textures.retire(entry);
						vk::bury(b, {.kind = vk::Dead::Kind::TextureSlot, .handle = entry.index()});
					}
				}
			}

			for (u32 i = 0; i < cold.attachment_view_count; ++i)
			{
				vk::bury(b,
				    {.kind = vk::Dead::Kind::ImageView, .handle = (u64)cold.attachment_views[i]});
			}
			if (cold.attachment_views != nullptr)
			{
				vk::bury(b, {.kind = vk::Dead::Kind::Memory, .handle = (u64)cold.attachment_views});
			}

			vk::bury(b, {.kind = vk::Dead::Kind::ImageView, .handle = (u64)hot.view});
			vk::bury(b, {.kind = vk::Dead::Kind::ImageView, .handle = (u64)hot.storage_view});
			vk::bury(b, {.kind = vk::Dead::Kind::Image,
			                .handle = (u64)hot.image,
			                .allocation = cold.allocation});
		}
	}

	namespace vk
	{
		VkImageLayout read_layout_of(TextureUsage usage)
		{
			return has_any(usage, TextureUsage::Storage) ? VK_IMAGE_LAYOUT_GENERAL
			                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		}

		TextureState uploaded_state(TextureUsage usage)
		{
			if (has_any(usage, TextureUsage::Sampled | TextureUsage::Storage))
			{
				return TextureState::ShaderRead;
			}
			if (has_any(usage, TextureUsage::DepthTarget))
			{
				return TextureState::DepthTarget;
			}
			if (has_any(usage, TextureUsage::ColorTarget))
			{
				return TextureState::ColorTarget;
			}

			return TextureState::CopyDst;
		}

		TextureHandle wrap_backbuffer(Backend& b, VkImage image, VkImageView view,
		    VkExtent2D extent, TextureFormat format, TextureUsage usage, SwapchainHandle swapchain,
		    u32 image_index)
		{
			b.resources_lock.lock();
			const TextureHandle handle =
			    b.textures.emplace(TextureHot{.image = image, .view = view});

			if (!handle.is_null())
			{
				TextureCold& cold = b.texture_cold[handle.index()];
				cold = TextureCold{};
				cold.extent = {extent.width, extent.height, 1};
				cold.api_format = format;
				cold.usage = usage;
				cold.swapchain = swapchain;
				cold.backbuffer_index = image_index;
			}

			b.resources_lock.unlock();
			return handle;
		}

		void release_backbuffer(Backend& b, TextureHandle handle)
		{
			b.resources_lock.lock();

			if (const TextureHot* hot = b.textures.get(handle))
			{
				bury(b, {.kind = Dead::Kind::ImageView, .handle = (u64)hot->view});
				(void)b.textures.retire(handle);
				bury(b, {.kind = Dead::Kind::TextureSlot, .handle = handle.index()});
			}

			b.resources_lock.unlock();
		}
	}

	TextureHandle create_texture(const TextureDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: texture '{}' has an invalid def", def.name);
			return {};
		}

		Natives natives;
		if (!create_natives(*b, def, natives))
		{
			destroy_natives(*b, natives);
			return {};
		}

		// Claim the hidden mip entries and the main entry together, so a full pool publishes
		// nothing. Descriptors are written before the handle can reach a recording job.
		TextureHandle mips[MAX_MIP_LEVELS] = {};
		TextureHandle handle;

		b->resources_lock.lock();
		if (claim_mips(*b, def, natives, nullptr, mips))
		{
			handle = b->textures.emplace();
			if (handle.is_null())
			{
				for (const TextureHandle mip : mips)
				{
					(void)b->textures.erase(mip);
				}
			}
			else
			{
				publish(*b, handle, def, natives, mips);
			}
		}
		b->resources_lock.unlock();

		if (handle.is_null())
		{
			EMBER_ERROR("gpu: texture pool full, '{}' refused", def.name);
			destroy_natives(*b, natives);
			return {};
		}

		vk::set_name(b->ctx, VK_OBJECT_TYPE_IMAGE, (u64)natives.image, def.name);
		return handle;
	}

	bool replace(TextureHandle handle, const TextureDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return false;
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: texture '{}' has an invalid def", def.name);
			return false;
		}

		const vk::TextureHot* hot = b->textures.get(handle);
		if (hot == nullptr)
		{
			EMBER_ERROR("gpu: replace of a stale texture with '{}'", def.name);
			return false;
		}

		const vk::TextureCold& cold = b->texture_cold[handle.index()];
		if (!cold.swapchain.is_null() || !cold.parent.is_null())
		{
			EMBER_ERROR("gpu: '{}' would replace a backbuffer or a hidden mip entry", def.name);
			return false;
		}

		if (same_shape(cold, def))
		{
			return true;
		}

		Natives natives;
		if (!create_natives(*b, def, natives))
		{
			destroy_natives(*b, natives);
			return false;
		}

		// Stop recording and wait for descriptor users before rewriting these slots.
		EMBER_ASSERT_MSG(!vk::any_frame_open(*b), "replace() with a new shape needs no frame open");
		vk::wait_frames(*b);

		// Claim extra mip entries before retiring anything so exhaustion preserves the old
		// texture. Reset old descriptors because type or usage changes can move bindings.
		const u32 kept_mips = has_any(def.usage, TextureUsage::Storage) ? def.mip_count : 1;
		TextureHandle mips[MAX_MIP_LEVELS] = {};

		b->resources_lock.lock();
		const bool claimed = claim_mips(*b, def, natives, &cold, mips);
		if (claimed)
		{
			vk::heap_reset_texture(*b, handle.index(), cold.type);
			retire_natives(*b, *hot, cold, kept_mips);
			publish(*b, handle, def, natives, mips);
		}
		b->resources_lock.unlock();

		if (!claimed)
		{
			EMBER_ERROR("gpu: texture pool full, '{}' keeps its old image", def.name);
			destroy_natives(*b, natives);
			return false;
		}

		vk::set_name(b->ctx, VK_OBJECT_TYPE_IMAGE, (u64)natives.image, def.name);
		return true;
	}

	void destroy(TextureHandle handle)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		// Resolve the handle under the lock so concurrent destroys cannot retire it twice.
		b->resources_lock.lock();

		const vk::TextureHot* hot = b->textures.get(handle);
		if (hot == nullptr)
		{
			b->resources_lock.unlock();
			return;
		}

		const vk::TextureCold& cold = b->texture_cold[handle.index()];
		EMBER_ASSERT_MSG(cold.parent.is_null(), "a hidden mip entry dies with its texture");

		if (!cold.swapchain.is_null())
		{
			EMBER_ASSERT_MSG(false, "a backbuffer is destroyed through its swapchain");
			b->resources_lock.unlock();
			return;
		}

		// Shaders have no generation, so retain the index until old GPU users finish.
		retire_natives(*b, *hot, cold, 1);
		(void)b->textures.retire(handle);
		vk::bury(*b, {.kind = vk::Dead::Kind::TextureSlot, .handle = handle.index()});

		b->resources_lock.unlock();
	}

	bool is_valid(TextureHandle handle)
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->textures.get(handle) != nullptr;
	}

	u32 storage_index(TextureHandle texture, u32 mip)
	{
		const vk::Backend* b = vk::backend();
		if (b == nullptr || b->textures.get(texture) == nullptr)
		{
			return 0;
		}

		const vk::TextureCold& cold = b->texture_cold[texture.index()];
		if (mip == 0)
		{
			return texture.index();
		}

		return mip < cold.mip_count ? cold.mip_storage[mip].index() : 0;
	}
}
