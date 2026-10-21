#include <ember/gpu/types.h>

#include <ember/core/macros.h>
#include <ember/math/scalar.h>

namespace ember::gpu
{
	namespace
	{
		// One row per TextureFormat, in enum order. Uncompressed formats are 1 by 1 blocks, so copy
		// math never special cases BC.
		constexpr FormatInfo FORMATS[] = {
		    {0, 1, 1, false, false},  // Undefined
		    {1, 1, 1, false, false},  // R8Unorm
		    {2, 1, 1, false, false},  // RG8Unorm
		    {4, 1, 1, false, false},  // RGBA8Unorm
		    {4, 1, 1, false, false},  // RGBA8Srgb
		    {4, 1, 1, false, false},  // BGRA8Unorm
		    {4, 1, 1, false, false},  // BGRA8Srgb
		    {2, 1, 1, false, false},  // R16Float
		    {4, 1, 1, false, false},  // RG16Float
		    {12, 1, 1, false, false}, // RGB32Float
		    {8, 1, 1, false, false},  // RGBA16Float
		    {4, 1, 1, false, false},  // R32Float
		    {8, 1, 1, false, false},  // RG32Float
		    {16, 1, 1, false, false}, // RGBA32Float
		    {4, 1, 1, false, false},  // R32Uint
		    {4, 1, 1, false, false},  // RG11B10Float
		    {4, 1, 1, false, false},  // RGB10A2Unorm
		    {4, 1, 1, true, false},   // D32Float
		    {8, 4, 4, false, true},   // BC1Unorm
		    {8, 4, 4, false, true},   // BC1Srgb
		    {16, 4, 4, false, true},  // BC3Unorm
		    {16, 4, 4, false, true},  // BC3Srgb
		    {8, 4, 4, false, true},   // BC4Unorm
		    {16, 4, 4, false, true},  // BC5Unorm
		    {16, 4, 4, false, true},  // BC6HFloat
		    {16, 4, 4, false, true},  // BC7Unorm
		    {16, 4, 4, false, true},  // BC7Srgb
		};

		static_assert(sizeof(FORMATS) / sizeof(FORMATS[0]) == (u32)TextureFormat::Count,
		    "FORMATS mirrors TextureFormat one to one");
		static_assert(FORMATS[(u32)TextureFormat::D32Float].depth);
		static_assert(FORMATS[(u32)TextureFormat::BC7Srgb].block_bytes == 16);
	}

	FormatInfo format_info(TextureFormat format)
	{
		EMBER_ASSERT(format < TextureFormat::Count);
		return FORMATS[(u32)format];
	}

	Extent3D mip_extent(Extent3D extent, u32 mip)
	{
		return {
		    max(extent.width >> mip, 1u),
		    max(extent.height >> mip, 1u),
		    max(extent.depth >> mip, 1u),
		};
	}

	u32 full_mip_count(Extent3D extent)
	{
		u32 largest = max(max(extent.width, extent.height), extent.depth);

		u32 count = 1;
		while (largest > 1)
		{
			largest >>= 1;
			++count;
		}

		return count;
	}

	u64 subresource_bytes(TextureFormat format, Extent3D extent, u32 mip)
	{
		const FormatInfo info = format_info(format);
		const Extent3D size = mip_extent(extent, mip);
		const u64 blocks_x = (size.width + info.block_width - 1) / info.block_width;
		const u64 blocks_y = (size.height + info.block_height - 1) / info.block_height;
		return blocks_x * blocks_y * size.depth * info.block_bytes;
	}

	u32 storage_array(TextureType type)
	{
		switch (type)
		{
			case TextureType::Texture2D:
				return 0;
			case TextureType::Texture2DArray:
			case TextureType::TextureCube:
				return 1;
			case TextureType::Texture3D:
				return 2;
			case TextureType::Count:
				break;
		}

		return 0;
	}

	bool is_valid(const TextureDef& def)
	{
		if (def.name == nullptr || def.format == TextureFormat::Undefined ||
		    def.format >= TextureFormat::Count || def.type >= TextureType::Count)
		{
			return false;
		}

		if (def.extent.width == 0 || def.extent.height == 0 || def.extent.depth == 0 ||
		    def.layers == 0 || def.usage == TextureUsage::None)
		{
			return false;
		}

		// A texture is a color thing or a depth thing, never both.
		const FormatInfo info = format_info(def.format);
		if (info.depth != has_any(def.usage, TextureUsage::DepthTarget) ||
		    (info.depth && has_any(def.usage, TextureUsage::ColorTarget | TextureUsage::Storage)))
		{
			return false;
		}

		// Compressed data comes from a cooker; the GPU never writes it.
		if (info.compressed &&
		    has_any(def.usage,
		        TextureUsage::Storage | TextureUsage::ColorTarget | TextureUsage::CopySrc))
		{
			return false;
		}

		switch (def.type)
		{
			case TextureType::Texture2D:
				if (def.layers != 1 || def.extent.depth != 1)
				{
					return false;
				}
				break;
			case TextureType::Texture2DArray:
				if (def.extent.depth != 1)
				{
					return false;
				}
				break;
			case TextureType::TextureCube:
				if (def.layers != 6 || def.extent.depth != 1)
				{
					return false;
				}
				break;
			case TextureType::Texture3D:
				if (def.layers != 1)
				{
					return false;
				}
				break;
			case TextureType::Count:
				return false;
		}

		if (def.mip_count == 0 || def.mip_count > MAX_MIP_LEVELS ||
		    def.mip_count > full_mip_count(def.extent))
		{
			return false;
		}

		// Multisampling is for attachments: one mip, 2D, a power of two up to eight.
		if (def.sample_count != 1)
		{
			const bool power_of_two = (def.sample_count & (def.sample_count - 1)) == 0;
			if (!power_of_two || def.sample_count > 8 || def.mip_count != 1 ||
			    def.type != TextureType::Texture2D ||
			    !has_any(def.usage, TextureUsage::ColorTarget | TextureUsage::DepthTarget))
			{
				return false;
			}
		}

		return true;
	}

	bool is_valid(const BufferDef& def)
	{
		if (def.name == nullptr || def.size == 0 || def.usage == BufferUsage::None ||
		    def.memory >= MemoryLocation::Count)
		{
			return false;
		}

		// Readback memory is what the GPU copies into; nothing else reads it.
		if (def.memory == MemoryLocation::Readback && def.usage != BufferUsage::CopyDst)
		{
			return false;
		}

		return true;
	}

	bool is_valid(const SamplerDef& def)
	{
		if (def.name == nullptr || def.min_lod > def.max_lod || def.max_anisotropy > 16)
		{
			return false;
		}

		// The spec forbids a reduction mode on a compare sampler.
		if (def.compare_enable && def.reduction != ReductionMode::WeightedAverage)
		{
			return false;
		}

		return def.min_filter < Filter::Count && def.mag_filter < Filter::Count &&
		       def.mip_filter < Filter::Count && def.address_u < AddressMode::Count &&
		       def.address_v < AddressMode::Count && def.address_w < AddressMode::Count &&
		       def.compare < CompareOp::Count && def.reduction < ReductionMode::Count &&
		       def.border < BorderColor::Count;
	}

	bool is_valid(const ShaderStageDef& def)
	{
		// SPIR-V is a stream of 32 bit words.
		return !def.code.empty() && def.code.size() % 4 == 0 &&
		       ((uintptr_t)def.code.data() & 3) == 0 && def.entry != nullptr;
	}

	bool is_valid(const GraphicsPipelineDef& def)
	{
		if (def.name == nullptr || !is_valid(def.vertex) || !is_valid(def.fragment))
		{
			return false;
		}

		if (def.color_count > MAX_COLOR_ATTACHMENTS ||
		    (def.color_count == 0 && def.depth_format == TextureFormat::Undefined))
		{
			return false;
		}

		for (u32 i = 0; i < def.color_count; ++i)
		{
			const TextureFormat format = def.color_formats[i];
			if (format == TextureFormat::Undefined || format >= TextureFormat::Count ||
			    format_info(format).depth)
			{
				return false;
			}
		}

		if (def.depth_format != TextureFormat::Undefined &&
		    (def.depth_format >= TextureFormat::Count || !format_info(def.depth_format).depth))
		{
			return false;
		}

		// Depth state without a depth attachment is a def that lies about itself.
		if ((def.depth_test || def.depth_write) && def.depth_format == TextureFormat::Undefined)
		{
			return false;
		}

		return def.depth_compare < CompareOp::Count && def.cull < CullMode::Count &&
		       def.front < FrontFace::Count && def.fill < FillMode::Count &&
		       def.blend < BlendPreset::Count && def.topology < PrimitiveTopology::Count;
	}

	bool is_valid(const ComputePipelineDef& def)
	{
		return def.name != nullptr && is_valid(def.shader);
	}
}
