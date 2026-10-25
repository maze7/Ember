#pragma once

// Records into lists opened by gpu::begin_command_list(). Each list has one
// writer, which may migrate between workers. End it before submission and stop
// using its pointer before frame-slot reuse. Callers supply resource barriers,
// cross-queue dependencies, and resource lifetimes.
//
// Passes set an attachment-sized viewport and scissor. Vulkan flips viewport Y
// to keep clip-space Y up. Redundant binds are skipped; changed constant offsets
// bind at the next draw or dispatch.
//
// A non-recording list asserts in debug builds and is ignored otherwise. Other
// pass, binding and resource preconditions remain the caller's responsibility.

#include <ember/gpu/types.h>

#include <type_traits>

namespace ember::gpu
{
	struct CommandList;

	// Selects pipeline stages, access masks and image layout for a transition.
	enum class TextureState : u8
	{
		// Discards previous contents. Use only as the source of a transition, such as
		// the first use of a new texture or an acquired backbuffer.
		Undefined,
		ColorTarget,
		DepthTarget,
		ShaderRead,

		// Shader storage access, including writes.
		ShaderWrite,
		CopySrc,
		CopyDst,

		// Final state before presentation. Use as the destination of a transition.
		Present,
		Count
	};

	struct TextureBarrier
	{
		TextureHandle texture = {};
		TextureState before = TextureState::Undefined;
		TextureState after = TextureState::Undefined;

		// Narrow the range when subresources change state independently.
		u32 base_mip = 0;
		u32 mip_count = ALL_MIPS;
		u32 base_layer = 0;
		u32 layer_count = ALL_LAYERS;

		// Moves ownership between queues with separate families. Record the same
		// barrier at the end of the releasing queue's list and at the start of the
		// acquiring queue's list, and make the acquiring submission wait on the
		// releasing one. When the two queues share a family the acquiring list
		// records nothing, so callers need not special-case such adapters.
		Queue from = Queue::Graphics;
		Queue to = Queue::Graphics;
	};

	// Selects pipeline stages and access masks; buffers have no image layout.
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

	// Integer formats take each channel's truncated whole number, so 1.0f clears
	// an R32Uint attachment to 1.
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

	// A null handle omits depth. The default clear is zero for reverse Z,
	// where the far plane has the smallest depth value.
	struct DepthAttachment
	{
		TextureHandle texture = {};
		u32 mip = 0;
		u32 layer = 0;
		LoadOp load = LoadOp::Clear;
		StoreOp store = StoreOp::Store;
		f32 clear_depth = 0.0f;
	};

	// Attachments define the render area. Callers supply equally sized subresources
	// and transition them to their target states before beginning the pass.
	struct RenderPassDef
	{
		Span<const ColorAttachment> colors = {};
		DepthAttachment depth = {};
	};

	// Native-compatible non-indexed draw arguments. Keep the layout stable so
	// shaders and cooked data can produce commands without repacking them.
	struct DrawIndirectArgs
	{
		u32 vertex_count = 0;
		u32 instance_count = 0;
		u32 first_vertex = 0;
		u32 first_instance = 0;
	};

	// Native-compatible indexed draw arguments, including a signed base-vertex offset.
	struct DrawIndexedIndirectArgs
	{
		u32 index_count = 0;
		u32 instance_count = 0;
		u32 first_index = 0;
		i32 base_vertex = 0;
		u32 first_instance = 0;
	};

	// Native-compatible dispatch arguments expressed as workgroup counts.
	struct DispatchIndirectArgs
	{
		u32 x = 1;
		u32 y = 1;
		u32 z = 1;
	};

	static_assert(sizeof(DrawIndirectArgs) == 16 && sizeof(DrawIndexedIndirectArgs) == 20 &&
	              sizeof(DispatchIndirectArgs) == 12);

	// Record outside render passes, using states this queue supports; transfer
	// lists have no shader stages. Texture barriers also change layouts and transfer
	// ownership between families. Buffers need no ownership transfer, but accesses
	// on different queues still need submission waits.
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

	// Orders earlier writes before later accesses on this queue. Does not change
	// image layouts or synchronize independent queues.
	void memory_barrier(CommandList* list);

	// Dispatches, copies, fills and explicit barriers belong outside render passes.
	void begin_render_pass(CommandList* list, const RenderPassDef& def);
	void end_render_pass(CommandList* list);

	// Framebuffer pixels from the top left, with Y increasing downward. Set within
	// a pass and supply a positive viewport height; the backend handles the flip.
	void set_viewport(CommandList* list, const Viewport& viewport);
	void set_scissor(CommandList* list, const Rect& scissor);

	void bind_pipeline(CommandList* list, GraphicsPipelineHandle pipeline);
	void bind_pipeline(CommandList* list, ComputePipelineHandle pipeline);

	// Copies a nonzero multiple of four bytes, up to PUSH_CONSTANT_BYTES, for all
	// shader stages. Every pipeline shares the layout, so a bind need not precede it.
	void push_constants(CommandList* list, const void* data, u32 size);

	template <typename T>
	void push_constants(CommandList* list, const T& data)
	{
		static_assert(std::is_trivially_copyable_v<T>, "push constants are raw bytes");
		static_assert(sizeof(T) <= PUSH_CONSTANT_BYTES, "a push block exceeds the contract");
		push_constants(list, &data, sizeof(T));
	}

	// Copies a nonempty block, at most MAX_CONSTANT_BLOCK_BYTES, into frame storage.
	// The slot keeps its offset until set again; the next draw or dispatch binds it.
	// Allocation failure logs and leaves the slot unchanged. Group slots by update
	// frequency using CONSTANTS_FRAME, CONSTANTS_PASS and CONSTANTS_DRAW.
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

	// Reads arguments at stride-byte intervals. The stride is a multiple of four
	// and at least the argument structure's size; extra bytes can hold draw metadata.
	void draw_indirect(CommandList* list, BufferHandle args, u64 offset, u32 draw_count,
	    u32 stride = sizeof(DrawIndirectArgs));

	void draw_indexed_indirect(CommandList* list, BufferHandle args, u64 offset, u32 draw_count,
	    u32 stride = sizeof(DrawIndexedIndirectArgs));

	// Takes the draw count from the GPU buffer at count_offset, capped by
	// max_draw_count. Requires Caps::indirect_count and the matching argument layout.
	void draw_indirect_count(CommandList* list, BufferHandle args, u64 offset, BufferHandle count,
	    u64 count_offset, u32 max_draw_count, u32 stride = sizeof(DrawIndirectArgs));

	void draw_indexed_indirect_count(CommandList* list, BufferHandle args, u64 offset,
	    BufferHandle count, u64 count_offset, u32 max_draw_count,
	    u32 stride = sizeof(DrawIndexedIndirectArgs));

	void dispatch(CommandList* list, u32 x, u32 y = 1, u32 z = 1);
	void dispatch_indirect(CommandList* list, BufferHandle args, u64 offset);

	void copy_buffer(CommandList* list, BufferHandle src, u64 src_offset, BufferHandle dst,
	    u64 dst_offset, u64 size);

	// Copies one whole mip and layer using tightly packed buffer data, including
	// block rows for BC formats. Transition the texture to CopyDst for uploads
	// or CopySrc for readback, and synchronize the buffer's copy access too.
	void copy_buffer_to_texture(CommandList* list, BufferHandle src, u64 src_offset,
	    TextureHandle dst, u32 mip, u32 layer);

	void copy_texture_to_buffer(CommandList* list, TextureHandle src, u32 mip, u32 layer,
	    BufferHandle dst, u64 dst_offset);

	// Fills whole u32 words, so offset and size must be multiples of four.
	// Record outside a render pass with the required copy dependencies in place.
	void fill_buffer(CommandList* list, BufferHandle dst, u64 offset, u64 size, u32 value);

	// Pair every begin with an end before ending the list. Keep name alive until
	// timing data is consumed. Capture labels remain available without timestamps.
	void begin_zone(CommandList* list, const char* name, u32 color = 0);
	void end_zone(CommandList* list);

	// Keep the scope inside the command list's recording lifetime.
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
