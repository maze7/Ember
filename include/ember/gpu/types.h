#pragma once

// The GPU module's types: handles, formats, the binding contract shaders are written against, and
// the Defs that describe buffers, textures, samplers, and pipelines. Nothing here requires a
// device, so code that only describes GPU state, material types and the cook tool, includes this
// and never links the backend.
//
// Every GPU object is named by a Handle into a pool the device owns, and a handle's index is also
// the resource's slot in the bindless descriptor heap: the u32 a shader indexes a texture or buffer
// with is handle.index(). Slot 0 of every array is a device-owned fallback, so the null handle, and
// a slot whose object has been released, read white or zero instead of crashing.

#include <ember/containers/span.h>
#include <ember/core/enum.h>
#include <ember/core/handle.h>
#include <ember/core/types.h>
#include <ember/math/geometry.h>

namespace ember
{
	// The tag types stay incomplete intentionally: user code copies, compares and stores handles,
	// and only the backend resolves one.
	struct Buffer;
	struct Texture;
	struct Sampler;
	struct GraphicsPipeline;
	struct ComputePipeline;
	struct Swapchain;

	using BufferHandle = Handle<Buffer>;
	using TextureHandle = Handle<Texture>;
	using SamplerHandle = Handle<Sampler>;
	using GraphicsPipelineHandle = Handle<GraphicsPipeline>;
	using ComputePipelineHandle = Handle<ComputePipeline>;
	using SwapchainHandle = Handle<Swapchain>;
}

namespace ember::gpu
{
	// The binding contract, shared with shaders/ember.slang. These are not tuning knobs: changing
	// one changes every shader and every backend.
	inline constexpr u32 MAX_COLOR_ATTACHMENTS = 8;
	inline constexpr u32 MAX_MIP_LEVELS = 16;
	inline constexpr u32 MAX_BINDLESS_SAMPLERS = 2048;
	inline constexpr u32 PUSH_CONSTANT_BYTES = 32;
	inline constexpr u32 MAX_CONSTANT_BLOCK_BYTES = 16384;
	inline constexpr u32 ALL_MIPS = ~0u;
	inline constexpr u32 ALL_LAYERS = ~0u;

	// Set 1 is three dynamic constant blocks, by how often they change. set_constants() fills one
	// from the frame's transient memory.
	inline constexpr u32 CONSTANT_SLOTS = 3;
	inline constexpr u32 CONSTANTS_FRAME = 0;
	inline constexpr u32 CONSTANTS_PASS = 1;
	inline constexpr u32 CONSTANTS_DRAW = 2;

	// Set 0 is the bindless heap: one array per binding, indexed by handle. Cube storage views are
	// 2D arrays (D3D12's rule for cube UAVs), so there are four sampled arrays and three storage.
	inline constexpr u32 BINDING_SAMPLED_2D = 0;
	inline constexpr u32 BINDING_SAMPLED_2D_ARRAY = 1;
	inline constexpr u32 BINDING_SAMPLED_CUBE = 2;
	inline constexpr u32 BINDING_SAMPLED_3D = 3;
	inline constexpr u32 BINDING_STORAGE_2D = 4;
	inline constexpr u32 BINDING_STORAGE_2D_ARRAY = 5;
	inline constexpr u32 BINDING_STORAGE_3D = 6;
	inline constexpr u32 BINDING_SAMPLERS = 7;
	inline constexpr u32 BINDING_STORAGE_BUFFERS = 8;
	inline constexpr u32 SAMPLED_ARRAY_COUNT = 4;
	inline constexpr u32 STORAGE_ARRAY_COUNT = 3;

	// The index a shader reads a resource's descriptor at. Mip 0 of a storage texture; deeper mips
	// come  from gpu::storage_index().
	template <typename Tag>
	u32 bindless_index(Handle<Tag> handle)
	{
		return handle.index();
	}

	// The queues work can be submitted to. When no dedicated family is present for compute or
	// transfer, that queue's submissions run on the graphics queue in order and Caps says so.
	enum class Queue : u8
	{
		Graphics,
		Compute,
		Transfer,
		Count
	};

	// A point on one queue's timeline. The value a submission signals when the GPU has finished it.
	// Zero covered nothing and is always complete. Any thread may ask after it or wait on it.
	struct Submission
	{
		u64 value = 0;
		Queue queue = Queue::Graphics;
	};

	// Order is ABI: values are serialized in cooked textures and pipeline caches. Append only; the
	// backend's format table static_asserts the count.
	enum class TextureFormat : u8
	{
		Undefined = 0,
		R8Unorm,
		RG8Unorm,
		RGBA8Unorm,
		RGBA8Srgb,
		BGRA8Unorm,
		BGRA8Srgb,
		R16Float,
		RG16Float,
		RGB32Float,
		RGBA16Float,
		R32Float,
		RG32Float,
		RGBA32Float,
		R32Uint,
		RG11B10Float,
		RGB10A2Unorm,
		D32Float,
		BC1Unorm,
		BC1Srgb,
		BC3Unorm,
		BC3Srgb,
		BC4Unorm,
		BC5Unorm,
		BC6HFloat,
		BC7Unorm,
		BC7Srgb,
		Count
	};

	// How a format packs: the bytes of one block and the texels a block covers, 1 by 1 for every
	// format that is not block compressed. Upload sizes and cooked mip chains are computed from it.
	struct FormatInfo
	{
		u8 block_bytes = 0;
		u8 block_width = 1;
		u8 block_height = 1;
		bool depth = false;
		bool compressed = false;
	};

	FormatInfo format_info(TextureFormat format);

	// The size of [mip] of a texture whose mip 0 is [extent], each side halved and never below one.
	Extent3D mip_extent(Extent3D extent, u32 mip);

	// Mips down to one texel on the largest side.
	u32 full_mip_count(Extent3D extent);

	// Bytes of one tightly packed subresource of a format, block rows included for BC formats.
	u64 subresource_bytes(TextureFormat format, Extent3D extent, u32 mip);

	enum class TextureUsage : u8
	{
		None = 0,
		Sampled = 1 << 0,
		Storage = 1 << 1,
		ColorTarget = 1 << 2,
		DepthTarget = 1 << 3,
		CopySrc = 1 << 4,
		CopyDst = 1 << 5,
	};

	EMBER_ENUM_BITWISE_OPS(TextureUsage);

	enum class TextureType : u8
	{
		Texture2D,
		Texture2DArray,
		TextureCube,
		Texture3D,
		Count
	};

	// Which storage array a texture's write descriptors live in, by type.
	u32 storage_array(TextureType type);

	// A texture is made empty; gpu::upload() or a render pass fills it. [name] is borrowed for the
	// call and shows in validation messages and captures. Depth is one unless Texture3D, the array
	// size is [layers], six for a cube. A sample count above one is for attachments only: no mips,
	// no uploads, Texture2D.
	struct TextureDef
	{
		const char* name = "texture";
		TextureType type = TextureType::Texture2D;
		Extent3D extent = {1, 1, 1};
		u32 layers = 1;
		TextureFormat format = TextureFormat::RGBA8Srgb;
		u32 mip_count = 1;
		u8 sample_count = 1;
		TextureUsage usage = TextureUsage::Sampled;
	};

	enum class BufferUsage : u8
	{
		None = 0,
		Index = 1 << 0,

		// Readable through the bindless storage buffer arrray, which is also how vertex data is
		// pulled: there is no vertex input state.
		Storage = 1 << 1,
		Indirect = 1 << 2,
		CopySrc = 1 << 3,
		CopyDst = 1 << 4,
	};

	EMBER_ENUM_BITWISE_OPS(BufferUsage);

	// Index width, bound with the buffer because every target API takes it at bind time.
	enum class IndexFormat : u8
	{
		U16,
		U32,
		Count
	};

	// Where a buffer's memory lives and how the CPU touches it.
	enum class MemoryLocation : u8
	{
		// VRAM. The CPU reaches it only through gpu::upload() and copies.
		DeviceLocal,

		// Host visible, persistently mapped and write combined: the CPU writes through
		// gpu::mapped() and never reads. Hazards against frames in flight are the caller's problem.
		Upload,

		// Host visible and cached: the GPU copies in, the CPU reads through gpu::mapped() once the
		// submission that copied has completed.
		Readback,
		Count
	};

	struct BufferDef
	{
		const char* name = "buffer";
		u64 size = 0;
		BufferUsage usage = BufferUsage::None;
		MemoryLocation memory = MemoryLocation::DeviceLocal;
	};

	enum class Filter : u8
	{
		Nearest,
		Linear,
		Count
	};

	enum class AddressMode : u8
	{
		Repeat,
		MirroredRepeat,
		ClampToEdge,
		ClampToBorder,
		Count
	};

	// The three colors core Vulkan guarantees. Arbitrary colors are an extension on vulkan and free
	// on D3D12. More can be added when something needs them.
	enum class BorderColor : u8
	{
		TransparentBlack,
		OpaqueBlack,
		OpaqueWhite,
		Count
	};

	// Shared by samplers and depth state
	enum class CompareOp : u8
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always,
		Count
	};

	// Min and Max need Caps::sampler_minmax, for Hi-Z pyramids. Creation falls back to
	// WeightedAverage with a log when it's not defined.
	enum class ReductionMode : u8
	{
		WeightedAverage,
		Min,
		Max,
		Count
	};

	// No upper LOD clamp, spelled the way Vulkan spells it.
	inline constexpr f32 LOD_NONE = 1000.0f;

	// Anisotropy of zero or one is off; higher is clamped to Caps::max_anisotropy. A compare
	// sampler is for shadow maps and excludes a reduction mode, which the spec forbids together.
	struct SamplerDef
	{
		const char* name = "sampler";
		Filter min_filter = Filter::Linear;
		Filter mag_filter = Filter::Linear;
		Filter mip_filter = Filter::Linear;
		AddressMode address_u = AddressMode::Repeat;
		AddressMode address_v = AddressMode::Repeat;
		AddressMode address_w = AddressMode::Repeat;
		f32 mip_lod_bias = 0.0f;
		f32 min_lod = 0.0f;
		f32 max_lod = LOD_NONE;
		u8 max_anisotropy = 0;
		bool compare_enable = false;
		CompareOp compare = CompareOp::Less;
		ReductionMode reduction = ReductionMode::WeightedAverage;
		BorderColor border = BorderColor::OpaqueBlack;
	};

	enum class PrimitiveTopology : u8
	{
		TriangleList,
		TriangleStrip,
		LineList,
		PointList,
		Count
	};

	enum class CullMode : u8
	{
		None,
		Back,
		Front,
		Count
	};

	// Which winding is the front, in the flipped viewport space a render pass establishes: D3D
	// style, Y up in clip space.
	enum class FrontFace : u8
	{
		CounterClockwise,
		Clockwise,
		Count
	};

	enum class FillMode : u8
	{
		Solid,

		// Needs Caps::wireframe; creation fails without it.
		Wireframe,
		Count
	};

	enum class BlendPreset : u8
	{
		Opaque,

		// Straight alpha: src.a lerps src over dst.
		AlphaBlend,

		// src.a scales an add; dst alpha is kept.
		Additive,

		// src already scaled by its alpha; the compositor preset.
		PremultipliedAlpha,
		Count
	};

	// One stage of a pipeline: SPIR-V, which is a stream of u32 words so [code] starts 4-byte
	// aligned and has a multiple of four bytes, and the entry point it runs. One blob may serve
	// several stages when it carries several entry points.
	struct ShaderStageDef
	{
		Span<const u8> code = {};
		const char* entry = "main";
	};

	// Depth added to every fragment as it is rasterized: [constant] in the format's smallest step
	// and [slope] times the primitive's depth change per pixel, capped at [clamp] when nonzero. A
	// shadow map pushes casters away from the light with it. Under reverse Z, away is negative.
	struct DepthBias
	{
		f32 constant = 0.0f;
		f32 slope = 0.0f;
		f32 clamp = 0.0f;
	};

	// Everything about a graphics pipeline. There is no vertex input state: vertex data is pulled
	// from storage buffers in the shader, so layouts belong to shader code and pipelines never
	// permute over them. Attachment formats are baked in. Depth is reverse Z: it clears to zero and
	// greater is closer.
	struct GraphicsPipelineDef
	{
		const char* name = "pipeline";
		ShaderStageDef vertex = {};
		ShaderStageDef fragment = {};

		TextureFormat color_formats[MAX_COLOR_ATTACHMENTS] = {};
		u32 color_count = 1;
		TextureFormat depth_format = TextureFormat::Undefined;

		bool depth_test = false;
		bool depth_write = false;
		CompareOp depth_compare = CompareOp::GreaterEqual;
		DepthBias depth_bias = {};

		// Clamps depth to the viewport range instead of clipping at the near and far planes, so a
		// shadow map keeps casters between the light and its near plane.
		bool depth_clamp = false;

		CullMode cull = CullMode::None;
		FrontFace front = FrontFace::CounterClockwise;
		FillMode fill = FillMode::Solid;
		BlendPreset blend = BlendPreset::Opaque;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
	};

	struct ComputePipelineDef
	{
		const char* name = "compute";
		ShaderStageDef shader = {};
	};

	// Whether a Def describes something the device can make. Creation checks these and refuses with
	// a log; a tool checks them to report before cooking.
	bool is_valid(const TextureDef& def);
	bool is_valid(const BufferDef& def);
	bool is_valid(const SamplerDef& def);
	bool is_valid(const ShaderStageDef& def);
	bool is_valid(const GraphicsPipelineDef& def);
	bool is_valid(const ComputePipelineDef& def);
}

namespace ember
{
	// How data files spell these: [Filter] and [Wrap] on material parameters, cull and blend on
	// material attributes.
	EMBER_ENUM_NAMES(gpu::Filter, "point", "linear");
	EMBER_ENUM_NAMES(gpu::AddressMode, "repeat", "mirror", "clamp", "border");
	EMBER_ENUM_NAMES(gpu::CullMode, "none", "back", "front");
	EMBER_ENUM_NAMES(gpu::BlendPreset, "opaque", "alpha", "additive", "premultiplied");
}
