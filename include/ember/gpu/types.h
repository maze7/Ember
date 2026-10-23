#pragma once

// Resource handles, formats, shader bindings, and descriptions of GPU state.
// These types let materials and tools describe resources without a live device.
//
// Buffer, texture, and sampler handles select pool slots whose indices also
// address the bindless descriptors. Shaders receive the index without its
// generation, so callers must keep those resources alive while the GPU uses them.
// Descriptor arrays start with fallback resources and are reset during retirement;
// this does not make stale indices safe after a slot is reused.

#include <ember/containers/span.h>
#include <ember/core/enum.h>
#include <ember/core/handle.h>
#include <ember/core/types.h>
#include <ember/math/geometry.h>

namespace ember
{
	// Incomplete tags distinguish handle types without exposing backend objects.
	// Callers can store and compare handles; only the owning pool resolves them.
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
	// Shared limits and binding numbers used by shaders/ember.slang and the backends.
	// Changing these requires updating both sides of the shader interface together.
	inline constexpr u32 MAX_COLOR_ATTACHMENTS = 8;
	inline constexpr u32 MAX_MIP_LEVELS = 16;
	inline constexpr u32 MAX_BINDLESS_SAMPLERS = 2048;
	inline constexpr u32 PUSH_CONSTANT_BYTES = 32;
	inline constexpr u32 MAX_CONSTANT_BLOCK_BYTES = 16384;
	inline constexpr u32 ALL_MIPS = ~0u;
	inline constexpr u32 ALL_LAYERS = ~0u;

	// Set 1 holds dynamic constant blocks grouped by update frequency.
	// set_constants() copies a block into the current frame's transient storage.
	inline constexpr u32 CONSTANT_SLOTS = 3;
	inline constexpr u32 CONSTANTS_FRAME = 0;
	inline constexpr u32 CONSTANTS_PASS = 1;
	inline constexpr u32 CONSTANTS_DRAW = 2;

	// Set 0 holds the bindless arrays, indexed by resource slot. Cube storage views
	// use the 2D-array binding, leaving four sampled arrays and three storage arrays.
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

	// Returns the descriptor slot without checking the handle's generation or usage.
	// Storage mips beyond zero have separate slots returned by gpu::storage_index().
	template <typename Tag>
	u32 bindless_index(Handle<Tag> handle)
	{
		return handle.index();
	}

	// Logical submission queues. Compute and transfer use the graphics queue when
	// the adapter has no dedicated family; Caps reports which queues are independent.
	enum class Queue : u8
	{
		Graphics,
		Compute,
		Transfer,
		Count
	};

	// A timeline point signaled after the submission's GPU work. A zero value names
	// no work and is already complete. It does not retain the submitted resources.
	struct Submission
	{
		u64 value = 0;
		Queue queue = Queue::Graphics;
	};

	// Keep existing values stable for cooked textures and cached pipeline data.
	// Append formats rather than renumbering them; the backend tables follow this order.
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

	// Describes the bytes and texel dimensions of one format block. Uncompressed
	// formats use 1x1 blocks, so upload and mip-size calculations share the same rules.
	// Integer formats hold whole numbers rather than normalized values, which
	// changes how a clear value is written.
	struct FormatInfo
	{
		u8 block_bytes = 0;
		u8 block_width = 1;
		u8 block_height = 1;
		bool depth = false;
		bool compressed = false;
		bool integer = false;
	};

	FormatInfo format_info(TextureFormat format);

	// Returns the extent at mip, halving each dimension without going below one.
	// The caller supplies a valid mip index.
	Extent3D mip_extent(Extent3D extent, u32 mip);

	// Counts the mip chain through the level where the largest dimension reaches one.
	u32 full_mip_count(Extent3D extent);

	// Bytes for one tightly packed mip and layer, including depth for 3D textures.
	// Compressed dimensions round up to whole texel blocks.
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

	// Selects the zero-based storage array for a texture type. Cubes share the
	// 2D-array storage binding.
	u32 storage_array(TextureType type);

	// Describes storage without initial pixels; upload or a render pass fills it.
	// name is borrowed during creation for diagnostics. Only 3D textures use depth
	// above one; array layers are explicit, with six layers for a cube. usage names
	// at least one shader or attachment use; a copy-only image would have no view.
	// 3D textures are filled by compute or copies, never used as attachments.
	struct TextureDef
	{
		const char* name = "texture";
		TextureType type = TextureType::Texture2D;
		Extent3D extent = {1, 1, 1};
		u32 layers = 1;
		TextureFormat format = TextureFormat::RGBA8Srgb;
		u32 mip_count = 1;
		TextureUsage usage = TextureUsage::Sampled;
	};

	enum class BufferUsage : u8
	{
		None = 0,
		Index = 1 << 0,

		// Makes the buffer visible through the bindless storage array. Vertex shaders
		// pull their data this way, so pipelines need no vertex-input layout.
		Storage = 1 << 1,
		Indirect = 1 << 2,
		CopySrc = 1 << 3,
		CopyDst = 1 << 4,
	};

	EMBER_ENUM_BITWISE_OPS(BufferUsage);

	// The index element width travels with the binding, allowing the same buffer
	// description to be used with either supported index format.
	enum class IndexFormat : u8
	{
		U16,
		U32,
		Count
	};

	// Chooses the allocation's memory properties and how callers access it from the CPU.
	enum class MemoryLocation : u8
	{
		// Device-local storage with no exposed CPU mapping. Fill it through uploads or copies.
		DeviceLocal,

		// Persistently mapped, coherent storage for CPU writes. Prefer sequential writes
		// and prevent overwriting bytes the GPU is still using. Placement and caching
		// depend on the adapter.
		Upload,

		// Mapped storage for CPU reads after a GPU copy. Wait for the copying submission
		// before reading it; the allocator prefers cached host memory.
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

	// Fixed border colors keep sampler descriptions portable across the backends.
	// Custom colors would need an additional API choice and backend support.
	enum class BorderColor : u8
	{
		TransparentBlack,
		OpaqueBlack,
		OpaqueWhite,
		Count
	};

	// Comparison functions shared by depth tests and comparison samplers.
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

	// Min and Max support reductions such as Hi-Z construction and require
	// Caps::sampler_minmax. Unsupported requests log and use WeightedAverage.
	enum class ReductionMode : u8
	{
		WeightedAverage,
		Min,
		Max,
		Count
	};

	// Default upper LOD limit, matching Vulkan's no-clamp sentinel.
	inline constexpr f32 LOD_NONE = 1000.0f;

	// Zero or one disables anisotropy; higher requests are limited by the adapter.
	// Comparison samplers require WeightedAverage reduction, so depth comparison
	// and min/max reduction cannot be combined in one sampler.
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

	// Front-facing winding after the viewport transform. Keep mesh winding and
	// viewport orientation consistent with the culling choice.
	enum class FrontFace : u8
	{
		CounterClockwise,
		Clockwise,
		Count
	};

	enum class FillMode : u8
	{
		Solid,

		// Requires Caps::wireframe; pipeline creation fails when it is unavailable.
		Wireframe,
		Count
	};

	enum class BlendPreset : u8
	{
		Opaque,

		// Composites straight-alpha source colors over the destination.
		AlphaBlend,

		// Adds alpha-weighted source color while preserving destination alpha.
		Additive,

		// Composites source colors that have already been multiplied by their alpha.
		PremultipliedAlpha,
		Count
	};

	// SPIR-V and its entry point for one shader stage. code starts on a four-byte
	// boundary and contains whole u32 words. Both views are borrowed during creation;
	// one module can serve several stages when it contains their entry points.
	struct ShaderStageDef
	{
		Span<const u8> code = {};
		const char* entry = "main";
	};

	// Constant and slope factors bias rasterized depth, with an optional clamp.
	// The constant factor is format-dependent rather than a world-space distance.
	// With reverse Z, a negative bias moves a shadow caster farther from the light.
	struct DepthBias
	{
		f32 constant = 0.0f;
		f32 slope = 0.0f;
		f32 clamp = 0.0f;
	};

	// Graphics state baked into a pipeline, including its attachment formats.
	// Vertex layouts stay in shader code because vertices are pulled from storage
	// buffers. The depth defaults use reverse Z: clear to zero and treat larger
	// values as closer. Match the pass's clear value to the chosen comparison.
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

		// Clamps depth instead of clipping at the near and far depth planes.
		// Shadow passes can use this to retain casters beyond the light's near plane.
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

	// Checks descriptions without creating device objects. Creation repeats these
	// checks and logs a rejected description, but adapter limits and native creation
	// can still fail. Tools can use this to reject malformed data before cooking.
	bool is_valid(const TextureDef& def);
	bool is_valid(const BufferDef& def);
	bool is_valid(const SamplerDef& def);
	bool is_valid(const ShaderStageDef& def);
	bool is_valid(const GraphicsPipelineDef& def);
	bool is_valid(const ComputePipelineDef& def);
}

namespace ember
{
	// Stable names used in material data for filters, addressing, culling, and blending.
	EMBER_ENUM_NAMES(gpu::Filter, "point", "linear");
	EMBER_ENUM_NAMES(gpu::AddressMode, "repeat", "mirror", "clamp", "border");
	EMBER_ENUM_NAMES(gpu::CullMode, "none", "back", "front");
	EMBER_ENUM_NAMES(gpu::BlendPreset, "opaque", "alpha", "additive", "premultiplied");
}
