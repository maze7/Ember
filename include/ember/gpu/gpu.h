#pragma once

// The GPU device. An abstraction layer above the Vulkan/DX12 APIS, driven by the frame loop and
// recorded into from jobs. It owns every GPU object in fixed pools, the bindless descriptor heap
// every pipeline is compiled against, each frame's transient and staging memory and command lists,
// and the queues' timelines.
//
// Any job or thread may create and destroy resources, upload, allocate transient memory, open,
// record, end and submit command lists, acquire a backbuffer and wait. Handles resolve lock-free,
// and the tables that mutate sit behind spin locks held for a few stores, and a command list owns
// its command pool, so the job recording it may resume on another worker between commands. A wait
// parks the calling fiber: the device polls the timeline first and otherwise hands the blocking
// wait to an IO thread and waits on a counter, so no worker blocks on the GPU or the display.

#include <ember/containers/span.h>
#include <ember/core/frame.h>
#include <ember/core/types.h>
#include <ember/gpu/types.h>
#include <ember/math/geometry.h>
#include <ember/platform/window.h>

namespace ember::gpu
{
	struct CommandList;

	// Validation layers cost a frame's worth of CPU; a release build pays for them only when asked.
#if NDEBUG
	inline constexpr bool VALIDATION_DEFAULT = false;
#else
	inline constexpr bool VALIDATION_DEFAULT = true;
#endif

	enum class AdapterPreference : u8
	{
		// A discrete GPU, or anything when there is none.
		Discrete,

		// THe integrated GPU, for power, or a laptop whose discrete GPU cannot present.
		Integrated,

		// The first adapter with the features the device needs.
		Any,
		Count
	};

	// Cpu is a software rasterizer, the last resort, but it lets a test run on a machine with no
	// GPU.
	enum class AdapterKind : u8
	{
		Other,
		Integrated,
		Discrete,
		Virtual,
		Cpu,
		Count
	};

	// Pool capacities, fixed at initialize() because a handle's index is also its bindless slot. A
	// destroyed resource keeps its slot until every frame that could read it has completed, so a
	// capacity covers live plus dying. Each is at most Handle::MAX_INDEX + 1 and samplers at most
	// MAX_BINDLESS_SAMPLERS.
	struct GpuLimits
	{
		u32 max_buffers = 16384;
		u32 max_textures = 16384;
		u32 max_samplers = 1024;
		u32 max_graphics_pipelines = 2048;
		u32 max_compute_pipelines = 512;
		u32 max_swapchains = 4;

		// Command lists one frame may open. Each owns a command pool and a slice of the timestamp
		// pool, so recording jobs never share either.
		u32 max_command_lists = 32;
	};

	struct GpuDef
	{
		const char* app_name = "Ember";

		// Validation layers and the debug messenger. Synchronization validation is an order of
		// magnitude slower than normal validation; opt in only when hunting a hazard. The break
		// asserts inside the messenger, so the debugger stops at the offending call.
		bool validation = VALIDATION_DEFAULT;
		bool sync_validation = false;
		bool break_on_validation_error = false;

		AdapterPreference adapter = AdapterPreference::Discrete;

		// No widnow system: Vulkan is loaded without the platform, no adapter needs to present,
		// and create_swapchain() refuses. For servers, tools and tests.
		bool headless = false;

		// Per frame in flight: the transient ring constants and per frame geometry live in, and the
		// staging ring upload() copies through. Memory that is spent refuses; it never grows.
		u64 transient_bytes = 32_mb;
		u64 staging_bytes = 64_mb;

		// Device memory the device may take from the adapter for buffers and textures, in blocks as
		// they are needed. Zero is the adapter's whole heap. A creation past the budget fails.
		u64 device_memory_budget = 0;

		GpuLimits limits = {};
	};

	// What the adapter can do and the alignments a caller honours. Filled by initialize(); the
	// backend keeps a copy of what it needs, so reading caps is never a hot path operation.
	struct Caps
	{
		char adapter_name[128] = {};
		u32 vendor_id = 0;
		u32 device_id = 0;
		u32 api_version = 0;
		AdapterKind adapter_kind = AdapterKind::Other;

		u64 device_memory_bytes = 0;

		// Resizable BAR or SAM: the transient ring lives in VRAM and the GPU reads it at full
		// speed.
		bool host_visible_device_memory = false;

		// Offsets into constant and storage buffers are multiples of these.
		u32 constant_offset_alignment = 256;
		u32 storage_offset_alignment = 256;

		// Buffer to texture copies: the row pitch and the buffer offset. One on Vulkan; D3D12
		// wants 256 and 512.
		u32 copy_row_pitch_alignment = 1;
		u32 copy_offset_alignment = 1;

		u32 max_texture_2d = 0;
		u32 max_texture_3d = 0;
		u32 max_texture_layers = 0;
		u32 max_color_attachments = 0;
		u32 max_indirect_draw_count = 0;
		u32 max_anisotropy = 1;
		u32 subgroup_size = 0;

		// Ticks to nanoseconds for GPU zones; zones() is empty without timestamps.
		f32 timestamp_period_ns = 0.0f;
		bool timestamps = false;

		bool wireframe = false;
		bool indirect_count = false;
		bool sampler_minmax = false;

		// A queue family of its own for compute, and one for transfer. Without one, that queue's
		// submissions run on the graphics queue in submission order.
		bool async_compute = false;
		bool async_transfer = false;
	};

	enum class PresentMode : u8
	{
		// FIFO: always available and never tears.
		VSync,

		// Low latency triple buffering; falls back to VSync with a log when not available.
		Mailbox,

		// May tear; falls back to VSync with a log.
		Immediate,
		Count
	};

	// The image count is clamped to what the surface allows.
	struct SwapchainDef
	{
		WindowHandle window = {};
		PresentMode present_mode = PresentMode::VSync;
		u32 image_count = 3;
	};

	// What acquire() hands back: the swapchain image the frame draws into, with the size and
	// format to size views and pipelines by.
	struct Backbuffer
	{
		TextureHandle texture = {};
		Extent2D extent = {};
		TextureFormat format = TextureFormat::Undefined;
	};

	// Frame lifetime memory the GPU reads: a slice of the frame's transient ring, host visible and
	// write combined, so write it in order and never read it back. Shaders reach it through the
	// buffer's bindless index at [offset]; indexing it as an array of T needs an allocation aligned
	// to sizeof(T), and its first element is then offset / sizeof(T). Good until the frame
	// completes.
	struct Transient
	{
		void* cpu = nullptr;
		BufferHandle buffer = {};
		u32 offset = 0;
		u32 size = 0;
	};

	// One queue submission: command lists that run in order, behind every earlier submission on the
	// same queue and behind the [waits], which are for another queue's work. Every list is ended
	// and belonds to the frame.
	struct SubmitDef
	{
		Span<CommandList* const> lists = {};
		Span<const Submission> waits = {};
		Queue queue = Queue::Graphics;
	};

	// One GPU zone of a completed frame. [name] is the pointer the zone was recorded with.
	struct ZoneTiming
	{
		const char* name = nullptr;
		u32 color = 0;
		u32 depth = 0;
		f32 duration_ms = 0.0f;
	};

	// A snapshot for debug views. Per frame values are from the last frame that ended.
	struct GpuStats
	{
		u64 transient_bytes = 0;
		u64 staging_bytes = 0;
		u64 device_memory_bytes = 0;
		u32 buffers = 0;
		u32 textures = 0;
		u32 samplers = 0;
		u32 pipelines = 0;
		u32 command_lists = 0;

		// Resources destroyed and waiting for the frames that could read them.
		u32 pending_destroys = 0;
	};

	// Loads the underlying API (Vulkan/DX12) through the platform, picks an adapter and creates the
	// device, the queues, the pools, the heap and a slot for each of frames::in_flight() frames.
	// Returns false when no adapter will do, logs the reason. Call from Main's fiber.
	bool initialize(const GpuDef& def = {});

	// Waits for the GPU, releases every resource still alive, which a debug build reports as a
	// leak, and unloads the underlying API (Vulkan/DX12). Call from Main's fiber, after the last
	// frame has completed.
	void shutdown();

	const Caps& caps();

	// The device was lost and nothing more will complete. Sticky: the game quits or reboots the
	// device.
	bool is_lost();

	// Opens the frame's slot. Parks until the frame that last held the slot has completed. A frame
	// with no GPU work still begins and ends, so that the slot's bookkeeping runs.
	void begin_frame(Frame& frame);

	// Closes the frame's slot: puts uploads not yet on a queue there, presents every swapchain the
	// frame acquired and signals the timeline. A backbuffer the frame never moved to
	// TextureState::Present is moved there first, so a frame that draws nothing still presents.
	void end_frame(Frame& frame);

	// Parks until the frame's GPU work is complete and ends its Stage::Gpu. Returns at once for a
	// frame that has completed, and for one that ended with no work.
	void wait_frame(Frame& frame);

	// Opens a command list of the frame's, named for captures and the profiler. Returns null and
	// logs if the frame has opened GpuLimits::max_command_lists.
	[[nodiscard]] CommandList* begin_command_list(const Frame& frame, const char* name);

	// Ends recording. Something, this job or another, then submits the list before the frame ends.
	void end_command_list(CommandList* list);

	// Puts the list on a queue, in order, behind every earlier submission on that queue and the
	// waits named. Uploads issued before this call land before it. Returns the point on the queue's
	// timeline where the lists complete.
	Submission submit(const Frame& frame, const SubmitDef& def);

	// Whether the GPU has finished a submission. As fresh as the last poll.
	bool is_complete(Submission submission);

	// Parks until the submission is complete.
	void wait(Submission submission);

	// Parks until every queue is idle. For a lost device or shutdown; not per frame.
	void wait_idle();

	[[nodiscard]] BufferHandle create_buffer(const BufferDef& def);
	[[nodiscard]] TextureHandle create_texture(const TextureDef& def);
	[[nodiscard]] SamplerHandle create_sampler(const SamplerDef& def);
	[[nodiscard]] GraphicsPipelineHandle create_graphics_pipeline(const GraphicsPipelineDef& def);
	[[nodiscard]] ComputePipelineHandle create_compute_pipeline(const ComputePipelineDef& def);

	// Kills the handle now; the object dies once every frame that could read it has completed.
	// Null handles are a no-op.
	void destroy(BufferHandle handle);
	void destroy(TextureHandle handle);
	void destroy(SamplerHandle handle);
	void destroy(GraphicsPipelineHandle handle);
	void destroy(ComputePipelineHandle handle);

	// Whether a handle still names a live object. Lock free, so any job may ask.
	bool is_valid(BufferHandle handle);
	bool is_valid(TextureHandle handle);
	bool is_valid(SamplerHandle handle);
	bool is_valid(GraphicsPipelineHandle handle);
	bool is_valid(ComputePipelineHandle handle);

	// The bindless index of one mip's storage descriptor. Mip 0 is the handle's own index; deeper
	// mips have entries of their own, made with the texture. Slot 0, the fallback, for a stale
	// handle or a mip the texture lacks.
	u32 storage_index(TextureHandle texture, u32 mip);

	// The persistent mapping of an Upload or Readback buffer. Null for DeviceLocal.
	void* mapped(BufferHandle buffer);

	// Frame lifetime memory the GPU reads. [alignment] is any value from 1 to 256 and the offset
	// is a multiple of it. A per worker bump over the frame's slice, so the fast path has no atomic
	// and no lock; a spent slice logs once and returns a Transient whose cpu is null.
	Transient allocate_transient(const Frame& frame, u32 size, u32 alignment);

	// Copies [data] into the frame's staging memory and schedules the copy into the buffer, or the
	// texture subresource tightly packed with block rows for BC formats, ahead of the frame's next
	// submit, so everything the frame submits afterwards sees it. Overlapping uploads to one range
	// in one frame are unordered. False, logged once, when the frame's staging is spent: try again
	// next frame.
	bool upload(const Frame& frame, BufferHandle buffer, u64 offset, Span<const u8> data);
	bool upload(const Frame& frame, TextureHandle texture, u32 mip, u32 layer, Span<const u8> data);

	// A swapchain for a window, on main's fiber. It follows the window's size on its own: an
	// acquire that finds the window changed remakes it and returns the new size. Null, logged, when
	// the window cannot show Vulkan or the pool is full.
	[[nodiscard]] SwapchainHandle create_swapchain(const SwapchainDef& def);
	void destroy_swapchain(SwapchainHandle swapchain);
	bool is_valid(SwapchainHandle swapchain);

	// The format of the swapchain's images, for the pipelines that draw into them.
	TextureFormat swapchain_format(SwapchainHandle swapchain);

	// The image the frame draws into, in TextureState::Undefined, which end_frame() presents once
	// the frame has moved it to TextureState::Present. A second acquire in one frame returns the
	// same image. Parks while the display holds every image. A null texture and a zero extent is
	// a window with no pixels to show, a minimized one: skip the render.
	Backbuffer acquire(const Frame& frame, SwapchainHandle swapchain);

	// The GPU zones a frame recorded, in recording order, resolved once the frame has completed
	// and good until its slot is reused. Empty before that, and on an adapter without timestamps.
	Span<const ZoneTiming> zones(const Frame& frame);

	GpuStats stats();

	// Validation messages since initialize(). A test asserts both stayed zero.
	u32 validation_errors();
	u32 validation_warnings();
}
