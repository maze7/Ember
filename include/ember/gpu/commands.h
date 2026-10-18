#pragma once

// Abstraction for recording GPU work. gpu::begin_command_list() opens a CommandList on a frame,
// the functions here fill it, gpu::end_command_liset() closes it and gpu::submit() hands it to a
// queue. A list is an opaque pointer rather than a handle because it lives inside one frame and
// sits on the hot path: every command goes straight to the native command buffer with no table
// lookup. The list owns its command pool, so the job recording it may resume on another worker
// between comands. One job records a list at a time; several jobs record several lists at once.
//
// State is explicit and small. Resources are bindless, so a draw names its textures and buffers
// through indices in its constants, not through binds. What is bound is a pipeline, an index buffer
// and the three constant slots, which set_constants() fills from the frame's transient memory.
//
// Barriers are the caller's responsibility. A resource moves between states where the recording
// says so, and nothing is tracked per draw. A render pass sets the viewport and scissor to its
// attachments, flipped so that clip space is Y up on every backend.
//
// Every command is a few stores and one Vulkan call. A command on a list that is not recording,
// ended or never opened, asserts in a debug build and does nothing in a release one.

#include <ember/gpu/types.h>

#include <type_traits>

namespace ember::gpu
{
	struct CommandList;

	// Where a textures memory sits for the next stretch of GPU work. One state names the stage,
	// access and layout the backend translates it to.
	enum class TextureState : u8
	{
		// Contents are garbage. Legal only as [before]: a fresh texture, or a backbuffer just
		// acquired.
		Undefined,
		ColorTarget,
		DepthTarget,
		ShaderRead,

		// Storage access from any shader stage
		ShaderWrite,
		CopySrc,
		CopyDst,

		// Legal only as [after]: the presentation engine takes over from here.
		Present,
		Count
	};

	struct TextureBarrier
	{
		TextureHandle texture = {};
		TextureState before = TextureState::Undefined;
		TextureState after = TextureState::Undefined;

		// The whole texture by default. Narrow when levels move independently, as a mip pyramid
		// downsample does.
		u32 base_mip = 0;
		u32 mip_count = ALL_MIPS;
		u32 base_layer = 0;
		u32 layer_count = ALL_LAYERS;
	};

	// Where a buffer's bytes sit for the next stretch of GPU work. Buffers have no layout, so a
	// state is only the stage and access to synchronize against.
	enum class BufferState : u8
	{
		ShaderRead,
		ShaderWrite,
		Index,
		Indirect,
		CopySrc,
		CopyDst,
		Count
	};

	struct BufferBarrier
	{
		BufferHandle buffer = {};
		BufferState before = BufferState::ShaderRead;
		BufferState after = BufferState::ShaderRead;
	};

	enum class LoadOp : u8
	{
		Load,
		Clear,
		DontCare,
		Count
	};

	enum class StoreOp : u8
	{
		Store,
		DontCare,
		Count
	};

	struct ClearColor
	{
		f32 r = 0.0f;
		f32 g = 0.0f;
		f32 b = 0.0f;
		f32 a = 1.0f;
	};

	struct ColorAttachment
	{
		TextureHandle texture = {};
		u32 mip = 0;
		u32 layer = 0;
		LoadOp load = LoadOp::Clear;
		StoreOp store = StoreOp::Store;
		ClearColor clear = {};
	};

	// A null texture is no depth attachment. Reverse Z: depth clears to zero, the far plane.
	struct DepthAttachment
	{
		TextureHandle texture = {};
		u32 mip = 0;
		u32 layer = 0;
		LoadOp load = LoadOp::Clear;
		StoreOp store = StoreOp::Store;
		f32 clear_depth = 0.0f;
	};

	// One render pass: its attachments, which also give it its size. Every attachment is the same
	// size and already in its target state when the pass begins.
	struct RenderPassDef
	{
		Span<const ColorAttachment> colors = {};
		DepthAttachment depth = {};
	};

	// Bit identical to VkDrawIndirectCommand and D3D12_DRAW_ARGUMENTS, so one buffer layout serves
	// every backend and may be baked into cooked data.
	struct DrawIndirectArgs
	{
		u32 vertex_count = 0;
		u32 instance_count = 0;
		u32 first_vertex = 0;
		u32 first_instance = 0;
	};

	// Bit identical to VkDrawIndexedIndirectCommand and D3D12_DRAW_INDEXED_ARGUMENTS.
	struct DrawIndexedIndirectArgs
	{
		u32 index_count = 0;
		u32 instance_count = 0;
		u32 first_index = 0;
		i32 base_vertex = 0;
		u32 first_instance = 0;
	};

	// Bit identical to VkDispatchIndirectCommand and D3D12_DISPATCH_ARGUMENTS.
	struct DispatchIndirectArgs
	{
		u32 x = 1;
		u32 y = 1;
		u32 z = 1;
	};

	static_assert(sizeof(DrawIndirectArgs) == 16 && sizeof(DrawIndexedIndirectArgs) == 20 &&
	              sizeof(DispatchIndirectArgs) == 12);

	// Moves resources between states, ordering everything before on the list's queue against
	// everything after.
	void barrier(CommandList* list, Span<const TextureBarrier> textures,
	    Span<const BufferBarrier> buffers = {});

	inline void barrier(CommandList* list, const TextureBarrier& texture)
	{
		barrier(list, Span<const TextureBarrier>(&texture, 1));
	}

	inline void barrier(CommandList* list, const BufferBarrier& buffer)
	{
		barrier(list, {}, Span<const BufferBarrier>(&buffer, 1));
	}

	// Everything before, visible to everything after. The sledgehammer, for cornering a hazard.
	void memory_barrier(CommandList* list);

	// Draws record between these. Compute and copies record outside them.
	void begin_render_pass(CommandList* list, const RenderPassDef& def);
	void end_render_pass(CommandList* list);

	// Narrower than the pass. In the flipped space: origin top left, Y down, in pixels, as D3D.
	void set_viewport(CommandList* list, const Viewport& viewport);
	void set_scissor(CommandList* list, const Rect& scissor);

	void bind_pipeline(CommandList* list, GraphicsPipelineHandle pipeline);
	void bind_pipeline(CommandList* list, ComputePipelineHandle pipeline);

	// Up to PUSH_CONSTANT_BYTES, seen by every stage of the bound pipeline.
	void push_constants(CommandList* list, const void* data, u32 size);

	template <typename T>
	void push_constants(CommandList* list, const T& data)
	{
		static_assert(std::is_trivially_copyable_v<T>, "push constants are raw bytes");
		static_assert(sizeof(T) <= PUSH_CONSTANT_BYTES, "a push block exceeds the contract");
		push_constants(list, &data, sizeof(T));
	}

	// Fills constant slot [slot] for the draws and dispatches that follow: copies [data] into the
	// frame's transient memory and rebinds the slot's offset at the next draw or dispatch. A slot
	// keeps its contents until set again. Use the slots by frequency: CONSTANTS_FRAME,
	// CONSTANTS_PASS, CONSTANTS_DRAW.
	void set_constants(CommandList* list, u32 slot, const void* data, u32 size);

	template <typename T>
	void set_constants(CommandList* list, u32 slot, const T& data)
	{
		static_assert(std::is_trivially_copyable_v<T>, "constants are raw bytes");
		static_assert(sizeof(T) <= MAX_CONSTANT_BLOCK_BYTES,
		    "a constant block exceeds what every adapter guarantees");
		set_constants(list, slot, &data, sizeof(T));
	}

	void bind_index_buffer(CommandList* list, BufferHandle buffer,
	    IndexFormat format = IndexFormat::U32, u64 offset = 0);

	void draw(CommandList* list, u32 vertex_count, u32 instance_count = 1, u32 first_vertex = 0,
	    u32 first_instance = 0);

	void draw_indexed(CommandList* list, u32 index_count, u32 instance_count = 1,
	    u32 first_index = 0, i32 base_vertex = 0, u32 first_instance = 0);

	// Args are packed at [stride], tightly by default; a wider stride carries per draw payload
	// beside them.
	void draw_indirect(CommandList* list, BufferHandle args, u64 offset, u32 draw_count,
	    u32 stride = sizeof(DrawIndirectArgs));

	void draw_indexed_indirect(CommandList* list, BufferHandle args, u64 offset, u32 draw_count,
	    u32 stride = sizeof(DrawIndexedIndirectArgs));

	// The GPU decides the count, read from [count] at [count_offset] and capped at
	// [max_draw_count]. Needs Caps::indirect_count.
	void draw_indirect_count(CommandList* list, BufferHandle args, u64 offset, BufferHandle count,
	    u64 count_offset, u32 max_draw_count, u32 stride = sizeof(DrawIndirectArgs));

	void draw_indexed_indirect_count(CommandList* list, BufferHandle args, u64 offset,
	    BufferHandle count, u64 count_offset, u32 max_draw_count,
	    u32 stride = sizeof(DrawIndexedIndirectArgs));

	void dispatch(CommandList* list, u32 x, u32 y = 1, u32 z = 1);
	void dispatch_indirect(CommandList* list, BufferHandle args, u64 offset);

	void copy_buffer(CommandList* list, BufferHandle src, u64 src_offset, BufferHandle dst,
	    u64 dst_offset, u64 size);

	// One subresource, tightly packed in the buffer at its offset, block rows for BC formats. The
	// texture is in CopyDst, or in CopySrc for the other direction.
	void copy_buffer_to_texture(CommandList* list, BufferHandle src, u64 src_offset,
	    TextureHandle dst, u32 mip, u32 layer);

	void copy_texture_to_buffer(CommandList* list, TextureHandle src, u32 mip, u32 layer,
	    BufferHandle dst, u64 dst_offset);

	// Offset and size are multiples of four: fill writes whole words.
	void fill_buffer(CommandList* list, BufferHandle dst, u64 offset, u64 size, u32 value);

	// Opens a debug label, for RenderDoc and Nsight, and a GPU timestamp pair the profiler reads
	// through gpu::zones() once the frame completes. Zones nest, and every begin has its end before
	// the list ends. [name] must outlive the frame, which a string literal does.
	void begin_zone(CommandList* list, const char* name, u32 color = 0);
	void end_zone(CommandList* list);

	// A zone that closes with its scope. Made through EMBER_GPU_ZONE, not by hand.
	class ZoneScope final
	{
	public:
		ZoneScope(CommandList* list, const char* name, u32 color = 0) : m_list(list)
		{
			begin_zone(list, name, color);
		}

		~ZoneScope()
		{
			end_zone(m_list);
		}

		ZoneScope(const ZoneScope&) = delete;
		ZoneScope& operator=(const ZoneScope&) = delete;

	private:
		CommandList* m_list;
	};
}

#define EMBER_GPU_ZONE_JOIN(a, b) a##b
#define EMBER_GPU_ZONE_NAME(line) EMBER_GPU_ZONE_JOIN(ember_gpu_zone_, line)
#define EMBER_GPU_ZONE(list, ...)                                                                  \
	::ember::gpu::ZoneScope EMBER_GPU_ZONE_NAME(__LINE__)(list, __VA_ARGS__)
