#pragma once

// Owns the device, fixed resource pools, bindless descriptors, queue timelines,
// and storage for frames in flight. Jobs may create resources, record and submit.
// Pool mutations are serialized; lock-free lookups do not keep resources alive.
// Callers order resource use and finish frame users before closing the frame.
//
// Each command list has one writer and a private pool, allowing fiber migration.
// The frame loop's fiber alone begins, ends and completes frames. Driver waits
// park scheduler fibers through I/O tasks when available; external callers and
// failed I/O submissions run the blocking call directly.

#include <ember/containers/span.h>
#include <ember/core/frame.h>
#include <ember/core/types.h>
#include <ember/gpu/types.h>
#include <ember/math/geometry.h>
#include <ember/platform/window.h>

#include <type_traits>

namespace ember::gpu
{
	struct CommandList;

#if NDEBUG
	inline constexpr bool VALIDATION_DEFAULT = false;
#else
	inline constexpr bool VALIDATION_DEFAULT = true;
#endif

	enum class AdapterPreference : u8
	{
		// Falls back to another supported kind when no discrete adapter is compatible.
		Discrete,

		Integrated,

		// Choose the first adapter that meets the device requirements.
		Any,
		Count
	};

	// Cpu identifies a software adapter.
	enum class AdapterKind : u8
	{
		Other,
		Integrated,
		Discrete,
		Virtual,
		Cpu,
		Count
	};

	// Fixed at initialization. Budget for fallbacks and pending retirement as well
	// as live resources: descriptor slots cannot be reused until their frames finish.
	// Limits fit Handle::MAX_INDEX + 1 and, for samplers, MAX_BINDLESS_SAMPLERS.
	struct GpuLimits
	{
		u32 max_buffers = 16384;
		u32 max_textures = 16384;
		u32 max_samplers = 1024;
		u32 max_graphics_pipelines = 2048;
		u32 max_compute_pipelines = 512;
		u32 max_swapchains = 4;

		// Each frame slot preallocates this many lists per queue, including their
		// private command pools and query ranges. Zero disables compute or transfer lists.
		u32 max_graphics_lists = 32;
		u32 max_compute_lists = 8;
		u32 max_transfer_lists = 4;
	};

	struct GpuDef
	{
		const char* app_name = "Ember";

		// Synchronization validation adds costly hazard tracking. Breaks occur inside
		// the callback, while the offending call is still on the debugger's stack.
		bool validation = VALIDATION_DEFAULT;
		bool sync_validation = false;
		bool break_on_validation_error = false;

		AdapterPreference adapter = AdapterPreference::Discrete;

		// Omits window-system integration and disables swapchain creation.
		bool headless = false;

		// Fixed bytes per frame slot. Both capacities must be divisible by 256 for
		// aligned ring refills. Allocations fail when a slice is full.
		u64 transient_bytes = 32_mb;
		u64 staging_bytes = 64_mb;

		// Backing-allocation limit applied to each device-local memory heap.
		// Zero uses the adapter heap's normal limit. This is separate from resource
		// pool capacities, and native allocation can still fail within either budget.
		u64 device_memory_budget = 0;

		GpuLimits limits = {};
	};

	// Captured at initialization. Resource creation also checks format and usage
	// restrictions and can fail within these limits.
	struct Caps
	{
		char adapter_name[128] = {};
		u32 vendor_id = 0;
		u32 device_id = 0;
		u32 api_version = 0;
		AdapterKind adapter_kind = AdapterKind::Other;

		u64 device_memory_bytes = 0;

		// Reports host-visible memory on a device-local heap larger than 256 MiB.
		// This is a diagnostic hint about the adapter; it does not identify a BAR
		// configuration or guarantee mapped allocation placement or bandwidth.
		bool host_visible_device_memory = false;

		// Required alignment for uniform-buffer and storage-buffer binding offsets.
		u32 constant_offset_alignment = 256;
		u32 storage_offset_alignment = 256;

		// Backend copy alignment values for rows and buffer offsets. Vulkan reports
		// preferred alignments here; copies must also meet their format's requirements.
		u32 copy_row_pitch_alignment = 1;
		u32 copy_offset_alignment = 1;

		u32 max_texture_2d = 0;
		u32 max_texture_3d = 0;
		u32 max_texture_layers = 0;
		u32 max_color_attachments = 0;
		u32 max_indirect_draw_count = 0;
		u32 max_anisotropy = 1;
		u32 subgroup_size = 0;

		// Nanoseconds per timestamp tick. Without timestamp support, zones have no
		// timing results even when capture labels are available.
		f32 timestamp_period_ns = 0.0f;
		bool timestamps = false;

		bool wireframe = false;
		bool indirect_count = false;
		bool sampler_minmax = false;

		// Dedicated queue families for compute and transfer. Without a dedicated
		// family, that logical queue shares the graphics queue and its submission order.
		bool async_compute = false;
		bool async_transfer = false;
	};

	enum class PresentMode : u8
	{
		// Queues images for synchronized presentation without tearing.
		VSync,

		// Presents the newest ready image instead of keeping older queued images.
		// Logs and falls back to VSync when the surface does not support it.
		Mailbox,

		// Presents without waiting for vertical synchronization and may tear.
		// Logs and falls back to VSync when unsupported.
		Immediate,
		Count
	};

	// image_count is clamped to the surface's supported range.
	struct SwapchainDef
	{
		WindowHandle window = {};
		PresentMode present_mode = PresentMode::VSync;
		u32 image_count = 3;
	};

	// A null texture means acquisition did not provide a drawable image.
	struct Backbuffer
	{
		TextureHandle texture = {};
		Extent2D extent = {};
		TextureFormat format = TextureFormat::Undefined;
	};

	// Mapped storage borrowed from the frame's transient slice. Write the bytes
	// before submitting their consumers; the slice is reused with the frame slot.
	// buffer is shared by the ring and must not be destroyed by the caller.
	// Shaders address the allocation through its bindless index and byte offset;
	// match the allocation's alignment and layout to the shader's access pattern.
	struct Transient
	{
		void* cpu = nullptr;
		BufferHandle buffer = {};
		u32 offset = 0;
		u32 size = 0;
	};

	// offset is a multiple of sizeof(T), so first_element() indexes data[0] when
	// shaders read the ring as StructuredBuffer<T>.
	template <typename T>
	struct TransientArray
	{
		T* data = nullptr;
		BufferHandle buffer = {};
		u32 offset = 0;
		u32 count = 0;

		u32 first_element() const
		{
			return offset / (u32)sizeof(T);
		}
	};

	// Ended command lists begun for this queue, and the dependencies to wait on.
	// Lists are submitted in the supplied order. Resource hazards still require
	// barriers, and uses on independent queues need the appropriate timeline waits.
	struct SubmitDef
	{
		Span<CommandList* const> lists = {};
		Span<const Submission> waits = {};
		Queue queue = Queue::Graphics;
	};

	// Resolved timing for one recorded zone. name is borrowed from begin_zone();
	// keep it alive while the result is being read.
	struct ZoneTiming
	{
		const char* name = nullptr;
		u32 color = 0;
		u32 depth = 0;
		f32 duration_ms = 0.0f;
	};

	// Ring usage comes from end_frame(); resource and device counts are sampled
	// by stats(). These figures are not a synchronized snapshot. Peaks help size
	// the fixed ring budgets.
	struct GpuStats
	{
		u64 transient_bytes = 0;
		u64 staging_bytes = 0;
		u64 transient_peak_bytes = 0;
		u64 staging_peak_bytes = 0;
		u64 device_memory_bytes = 0;
		u32 buffers = 0;
		u32 textures = 0;
		u32 samplers = 0;
		u32 pipelines = 0;
		u32 command_lists = 0;

		// Retirement entries awaiting frame completion. One destroyed resource can
		// contribute several entries, so this is not a count of resource handles.
		u32 pending_destroys = 0;
	};

	// Selects a compatible adapter and creates the device and its fixed storage.
	// Call from main's fiber after memory, jobs, and frames are initialized, with
	// no concurrent device users. Logs setup failures and returns false.
	bool initialize(const GpuDef& def = {});

	// Tears down the device from main's fiber, waiting for GPU idle before releasing
	// backend state. Finish all CPU users first. Debug builds report resources
	// the caller left alive; shutdown also releases those resources.
	void shutdown();

	const Caps& caps();

	// Reports an observed device-loss error. The flag stays set for this device
	// lifetime; stop submitting work and tear down before attempting a new device.
	bool is_lost();

	// Opens the frame's slot and resets its command pools and mapped slices after
	// waiting for any remaining GPU use. Return and retire the previous owner
	// through wait_oldest() first. Publish the initialized frame before jobs use it,
	// and pair begin/end even for empty frames. Call this, end_frame() and
	// wait_oldest() on the frame loop's fiber, which alone completes frames.
	void begin_frame(Frame& frame);

	// Closes the frame after all its recording, upload, acquire, and submit calls
	// have finished. Submits any uploads still pending, transitions acquired images
	// to Present, submits the final graphics work, and requests presentation.
	// Callers supply dependencies on independent queues. Stage::Present measures
	// the CPU handoff; Stage::Gpu ends when the frame's completion is processed.
	void end_frame(Frame& frame);

	// Waits for and completes the oldest held frame, then returns it once for the
	// loop to read its zones and retire. Returns null when none remain. Call when
	// all frame slots are live, and drain at shutdown. Runs on the frame loop's
	// fiber and tracks GPU work, not completion of presentation.
	Frame* wait_oldest();

	// Opens a list for the named logical queue; submit it to that same queue.
	// Keep name alive through the frame. Exhaustion logs and returns null. The
	// caller owns recording until handing the ended list to a submitter.
	[[nodiscard]] CommandList* begin_command_list(const Frame& frame, const char* name,
	    Queue queue = Queue::Graphics);

	// Ends recording after all passes and zones are closed. Submit the list once,
	// from this job or another, before ending its frame.
	void end_command_list(CommandList* list);

	// Submits lists after the frame's uploads recorded so far. Upload barriers cover
	// later graphics work; a separate queue waits on their graphics timeline point.
	// Pass other submission dependencies in waits. Returns this queue's timeline point.
	Submission submit(const Frame& frame, const SubmitDef& def);

	inline Submission submit(const Frame& frame, CommandList* list)
	{
		SubmitDef def;
		def.lists = Span<CommandList* const>(&list, 1);
		return submit(frame, def);
	}

	// Starts pending uploads without submitting a list. Returns the graphics point
	// covering every upload recorded so far this frame, or zero if there were none.
	// submit() also flushes uploads and handles waits for separate queues.
	Submission submit_uploads(const Frame& frame);

	// Checks cached completion and queries the queue timeline when needed.
	// A zero submission is already complete.
	bool is_complete(Submission submission);

	// Waits on the submission's timeline. Scheduler callers use the I/O path when
	// available; the direct fallback can block the calling thread.
	void wait(Submission submission);

	// Performs a device-wide wait and drains pending retirement. Reserve this for
	// teardown or recovery, with CPU submitters stopped, rather than normal frames.
	// Runs on the frame loop's fiber.
	void wait_idle();

	[[nodiscard]] BufferHandle create_buffer(const BufferDef& def);
	[[nodiscard]] TextureHandle create_texture(const TextureDef& def);
	[[nodiscard]] SamplerHandle create_sampler(const SamplerDef& def);
	[[nodiscard]] GraphicsPipelineHandle create_graphics_pipeline(const GraphicsPipelineDef& def);
	[[nodiscard]] ComputePipelineHandle create_compute_pipeline(const ComputePipelineDef& def);

	// Invalidates the handle and defers native destruction through frame retirement.
	// Buffer, texture, and sampler descriptor slots wait too; pipeline pool slots
	// can be reused immediately. Stop CPU uses before destroying a resource and
	// keep GPU uses within the tracked frames. Null handles are ignored.
	void destroy(BufferHandle handle);
	void destroy(TextureHandle handle);
	void destroy(SamplerHandle handle);
	void destroy(GraphicsPipelineHandle handle);
	void destroy(ComputePipelineHandle handle);

	// Checks whether the pool still contains this handle's generation. The lookup
	// takes no lock, but its result does not pin the object against destruction.
	bool is_valid(BufferHandle handle);
	bool is_valid(TextureHandle handle);
	bool is_valid(SamplerHandle handle);
	bool is_valid(GraphicsPipelineHandle handle);
	bool is_valid(ComputePipelineHandle handle);

	// Keeps the handle and bindless indices, including retained storage mip indices.
	// Pipelines swap atomically from any thread; recording sees either version, and
	// the old pipeline retires after its frames complete.
	//
	// Texture, buffer and sampler descriptor changes wait for all frames in flight.
	// Call between frames on the frame loop's fiber with no frame open. For streaming
	// that must overlap rendering, create a new resource and switch at the asset layer.
	// A texture or buffer with the same shape keeps its storage and returns true
	// without waiting; upload new contents separately. Failure leaves the old resource.
	bool replace(TextureHandle handle, const TextureDef& def);
	bool replace(BufferHandle handle, const BufferDef& def);
	bool replace(SamplerHandle handle, const SamplerDef& def);
	bool replace(GraphicsPipelineHandle handle, const GraphicsPipelineDef& def);
	bool replace(ComputePipelineHandle handle, const ComputePipelineDef& def);

	// Requires a texture created with TextureUsage::Storage. Mip zero returns its
	// pool index; deeper mips have hidden entries created with it. Returns zero for
	// a stale handle or an out-of-range mip. Keep the parent texture alive while
	// using an index; this function does not validate its storage usage.
	u32 storage_index(TextureHandle texture, u32 mip);

	// Returns an Upload or Readback buffer's persistent mapping. Null for device-local
	// storage or an invalid handle. The caller synchronizes CPU access with GPU use.
	void* mapped(BufferHandle buffer);

	// Borrows bytes from the open frame's transient slice. Request a power-of-two
	// alignment from 1 through 256. Workers bump local chunks and use a shared counter
	// for refills; external threads use that counter directly. Failure returns an empty
	// allocation, with exhaustion logged once per slice. Storage lasts until slot reuse.
	Transient allocate_transient(const Frame& frame, u32 size, u32 alignment);

	// Reserves up to sizeof(T) - 1 padding bytes so first_element() is exact even
	// for strides that are not powers of two. Returns empty for zero count, byte
	// counts beyond u32, or allocation failure. Elements are not constructed.
	template <typename T>
	TransientArray<T> allocate_transient(const Frame& frame, u32 count)
	{
		static_assert(std::is_trivially_copyable_v<T>, "transient memory is raw bytes");
		static_assert(alignof(T) <= 256, "the ring aligns to at most 256 bytes");
		constexpr u32 STRIDE = (u32)sizeof(T);

		const u64 bytes = (u64)count * STRIDE + STRIDE - 1;
		if (count == 0 || bytes > 0xffffffffu)
		{
			return {};
		}

		const Transient raw = allocate_transient(frame, (u32)bytes, alignof(T));
		if (raw.cpu == nullptr)
		{
			return {};
		}

		const u32 start = (raw.offset + STRIDE - 1) / STRIDE * STRIDE;
		return {(T*)((u8*)raw.cpu + (start - raw.offset)), raw.buffer, start, count};
	}

	// Copies data into staging before returning. Requires an open frame; the next
	// submit(), submit_uploads() or end_frame() sends the batch on the graphics queue.
	// Its barriers cover later graphics consumers; submit() waits for it on separate
	// queues. Order prior uses and overlapping writes explicitly.
	//
	// Texture data replaces one whole tightly packed mip and layer, including block
	// rows. The result is ShaderRead for shader access, otherwise its attachment
	// state or CopyDst. Transfer ownership with a barrier before another family
	// uses the texture. Invalid requests or exhausted staging/batches return false;
	// capacity failures can be retried in a later frame.
	bool upload(const Frame& frame, BufferHandle buffer, u64 offset, Span<const u8> data);
	bool upload(const Frame& frame, TextureHandle texture, u32 mip, u32 layer, Span<const u8> data);

	// Creates a window's swapchain. Create and destroy it on main's fiber, and end
	// any frame that acquired it before destruction. Keep the window alive until
	// then. Acquisition handles surface changes and can recreate the images.
	// Creation failure logs and returns a null handle.
	[[nodiscard]] SwapchainHandle create_swapchain(const SwapchainDef& def);
	void destroy_swapchain(SwapchainHandle swapchain);
	bool is_valid(SwapchainHandle swapchain);

	TextureFormat swapchain_format(SwapchainHandle swapchain);

	// Repeated calls in one frame return the same image, initially Undefined;
	// end_frame() transitions it to Present. Waiting uses I/O when available, and
	// only main's fiber queries window size directly. Skip rendering on a null
	// texture, which can mean a zero-sized surface or an acquisition failure.
	Backbuffer acquire(const Frame& frame, SwapchainHandle swapchain);

	// Read timings once wait_oldest() has handed the frame back, before retiring it.
	// The returned span borrows slot storage until the slot is reused. Empty when
	// timing is unavailable or the frame no longer owns its slot.
	Span<const ZoneTiming> zones(const Frame& frame);

	GpuStats stats();

	// Validation callback counts since initialize(), retained after shutdown.
	u32 validation_errors();
	u32 validation_warnings();
}
