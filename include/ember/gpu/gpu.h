#pragma once

// Owns the GPU device, fixed resource pools, bindless descriptors, queue timelines,
// and the command and mapped storage reused by frames in flight.
//
// Resource operations and command recording can run from jobs. Tables serialize
// their mutations, while handle lookups check pool generations without a lock.
// A successful lookup does not keep an object alive. Callers order resource use
// and frame retirement, and finish all frame users before closing or reusing it.
// Each command list has one writer and its own pool, so recording can continue
// on another worker after a fiber wait.
//
// Driver waits use an I/O task when a scheduler caller can submit one. The fiber
// parks on its counter. External threads and failed I/O submissions run the call
// directly, so that fallback can block the caller.

#include <ember/containers/span.h>
#include <ember/core/frame.h>
#include <ember/core/types.h>
#include <ember/gpu/types.h>
#include <ember/math/geometry.h>
#include <ember/platform/window.h>

namespace ember::gpu
{
	struct CommandList;

	// Enable validation by default in debug builds. Release builds can opt in
	// when investigating driver usage without making it a normal runtime cost.
#if NDEBUG
	inline constexpr bool VALIDATION_DEFAULT = false;
#else
	inline constexpr bool VALIDATION_DEFAULT = true;
#endif

	enum class AdapterPreference : u8
	{
		// Prefer a compatible discrete adapter, falling back to other supported kinds.
		Discrete,

		// Prefer an integrated adapter when it meets the device requirements.
		Integrated,

		// Choose the first adapter that meets the device requirements.
		Any,
		Count
	};

	// The selected adapter's class. Cpu identifies a software device, which can
	// run tools and tests without a hardware GPU.
	enum class AdapterKind : u8
	{
		Other,
		Integrated,
		Discrete,
		Virtual,
		Cpu,
		Count
	};

	// Capacities are fixed at initialization. Buffers, textures, and samplers use
	// pool indices as descriptor slots, so retired entries cannot be reused until
	// their frame stamps pass. Budget for fallbacks and pending retirement as well
	// as live objects. Limits fit Handle::MAX_INDEX + 1; sampler slots also fit
	// MAX_BINDLESS_SAMPLERS.
	struct GpuLimits
	{
		u32 max_buffers = 16384;
		u32 max_textures = 16384;
		u32 max_samplers = 1024;
		u32 max_graphics_pipelines = 2048;
		u32 max_compute_pipelines = 512;
		u32 max_swapchains = 4;

		// Maximum lists opened by one frame. Each has its own command pool and query
		// range so concurrent recorders do not share that recording storage.
		u32 max_command_lists = 32;
	};

	struct GpuDef
	{
		const char* app_name = "Ember";

		// Development checks and their debug messenger. Synchronization validation
		// adds hazard tracking and can be expensive. The break option stops in the
		// callback so a debugger can inspect the offending call.
		bool validation = VALIDATION_DEFAULT;
		bool sync_validation = false;
		bool break_on_validation_error = false;

		AdapterPreference adapter = AdapterPreference::Discrete;

		// Initializes without window-system integration or presentation requirements.
		// Swapchain creation is unavailable; useful for servers, tools, and tests.
		bool headless = false;

		// Fixed bytes per frame slot for transient data and staged uploads.
		// Keep both capacities divisible by 256 so each slice starts at the ring's
		// refill alignment. Allocations that do not fit fail instead of growing a ring.
		u64 transient_bytes = 32_mb;
		u64 staging_bytes = 64_mb;

		// Backing-allocation limit applied to each device-local memory heap.
		// Zero uses the adapter heap's normal limit. This is separate from resource
		// pool capacities, and native allocation can still fail within either budget.
		u64 device_memory_budget = 0;

		GpuLimits limits = {};
	};

	// Adapter features and limits captured during initialization. Creation can
	// impose further format and usage restrictions; a reported limit alone does
	// not guarantee that a particular resource can be allocated.
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

	// Requests a swapchain for a window. The backend clamps image_count to the
	// surface's supported range.
	struct SwapchainDef
	{
		WindowHandle window = {};
		PresentMode present_mode = PresentMode::VSync;
		u32 image_count = 3;
	};

	// The acquired image and the dimensions and format needed to render into it.
	// An empty texture means acquisition did not provide a drawable image.
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

	// Ended command lists belonging to this frame and the dependencies to wait on.
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

	// Diagnostic usage figures. Ring usage is cached at end_frame(); resource
	// counts and device allocations are sampled by stats(). They do not form
	// one synchronized snapshot of concurrent activity.
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

	// Opens the frame's slot, waiting for its previous GPU use before resetting
	// command pools and mapped slices. Publish the initialized frame before jobs
	// use it. Pair begin and end even when no application work is recorded.
	void begin_frame(Frame& frame);

	// Closes the frame after all its recording, upload, acquire, and submit calls
	// have finished. Flushes pending uploads, transitions acquired images to Present,
	// submits the final graphics work, and requests presentation. Callers supply
	// dependencies on independent queues. Stage::Present measures the CPU handoff;
	// Stage::Gpu ends when the frame's timeline completion is processed.
	void end_frame(Frame& frame);

	// Waits for an ended frame's recorded queue values and processes its completion.
	// This tracks GPU work rather than the image reaching the display.
	void wait_frame(Frame& frame);

	// Opens a list in the frame's fixed recording storage. Keep name alive through
	// the frame for diagnostics. Exhaustion logs and returns null; recording is
	// exclusive to the caller until the ended list is handed to a submitter.
	[[nodiscard]] CommandList* begin_command_list(const Frame& frame, const char* name);

	// Ends recording after all passes and zones are closed. Submit the list once,
	// from this job or another, before ending its frame.
	void end_command_list(CommandList* list);

	// Submits the lists and returns their queue timeline point. Graphics submissions
	// flush the pending upload batch before the supplied lists. Order uploads before
	// this call when they must be included, and provide waits for cross-queue uses.
	Submission submit(const Frame& frame, const SubmitDef& def);

	// Checks cached completion and queries the queue timeline when needed.
	// A zero submission is already complete.
	bool is_complete(Submission submission);

	// Waits on the submission's timeline. Scheduler callers use the I/O path when
	// available; the direct fallback can block the calling thread.
	void wait(Submission submission);

	// Performs a device-wide wait and drains pending retirement. Reserve this for
	// teardown or recovery, with CPU submitters stopped, rather than normal frames.
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

	// Copies data into staging storage before returning, then records a GPU copy
	// ahead of the next graphics submission that flushes the batch. Consumers on
	// independent queues wait on that submission. Texture data replaces one whole,
	// tightly packed mip and layer, including block rows, and ends in ShaderRead.
	// Order prior uses and avoid overlapping writes without explicit dependencies.
	// Invalid requests, exhausted staging, or a full upload-batch table return false;
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

	// Returns the image format used by the swapchain so pipelines can match it.
	TextureFormat swapchain_format(SwapchainHandle swapchain);

	// Acquires a drawable image for this frame, initially tracked as Undefined.
	// Repeated acquisition in one frame returns the same image; end_frame() moves it
	// to Present if needed. Waiting uses the I/O path when available. Main-fiber calls
	// can query the window size directly. An empty result can mean a zero-sized
	// surface or an acquisition failure; skip rendering when no texture is returned.
	Backbuffer acquire(const Frame& frame, SwapchainHandle swapchain);

	// Read timings after the frame's completion handling has finished. The returned
	// span borrows slot storage until reuse and does not synchronize with concurrent
	// polling. Empty when timing is unavailable or the frame no longer owns its slot.
	Span<const ZoneTiming> zones(const Frame& frame);

	GpuStats stats();

	// Error and warning counts reported by the validation callback since initialize().
	// Useful for checking that a run completed without validation messages.
	u32 validation_errors();
	u32 validation_warnings();
}
