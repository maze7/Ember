#pragma once

// Shared Vulkan state, allocated on the Graphics heap between initialize() and shutdown().
// Configuration is fixed at boot. Queue locks and atomics track submission and completion.
// Frame slots own command lists, uploads and ring slices until all their queue values pass.
//
// resources_lock serializes pool mutations. Recording uses lock-free lookups and relies on
// callers to keep resources alive. Destruction retires native objects and descriptor indices
// until the frames that could use them finish.

#include <gpu/vulkan/common.h>

#include <ember/containers/pool.h>
#include <ember/containers/vector.h>
#include <ember/core/jobs.h>
#include <ember/core/spin_mutex.h>
#include <ember/gpu/commands.h>
#include <ember/gpu/gpu.h>

#include <vk_mem_alloc.h>

#include <atomic>

namespace ember::gpu
{
	namespace vk
	{
		// A list can time this many zones. Further zones still get debug labels.
		inline constexpr u32 MAX_ZONES = 64;

		inline constexpr u32 QUEUE_COUNT = (u32)Queue::Count;
		inline constexpr u32 MAX_SWAPCHAINS = 8;
	}

	// One recording writer owns the command buffer, its private pool, and cached binding state
	// between begin_command_list() and end_command_list(). The slot keeps them for reuse.
	struct CommandList
	{
		// The boot-created pool restricts recording and submission to this queue.
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		Queue queue = Queue::Graphics;

		// Together these locate the list's timestamp queries as well as its frame slot. The
		// index is unique across the slot's queue tables, with the finish list last.
		u32 slot = 0;
		u32 index = 0;

		// Ending permits one submission. Slot reuse resets the list for its next frame.
		u64 frame_index = 0;
		bool open = false;
		bool ended = false;
		bool submitted = false;
		bool inside_pass = false;

		const char* name = nullptr;

		// Cached to skip redundant Vulkan binds.
		GraphicsPipelineHandle graphics_pipeline = {};
		ComputePipelineHandle compute_pipeline = {};
		VkBuffer index_buffer = VK_NULL_HANDLE;
		u64 index_offset = 0;
		VkIndexType index_type = VK_INDEX_TYPE_UINT32;

		// set_constants() changes these offsets without binding them immediately. Graphics and
		// compute each consume their own dirty flag before the next draw or dispatch.
		u32 constant_offsets[CONSTANT_SLOTS] = {};
		bool constants_dirty_graphics = false;
		bool constants_dirty_compute = false;

		// submit() folds these into pending presents in submission order. Recording order
		// can differ and would give end_frame() the wrong state. One image per swapchain
		// is acquired each frame.
		struct BackbufferUse
		{
			SwapchainHandle swapchain = {};
			u32 image = 0;
			TextureState state = TextureState::Undefined;
		};

		BackbufferUse backbuffers[vk::MAX_SWAPCHAINS] = {};
		u32 backbuffer_count = 0;

		// CPU labels and nesting are combined with timestamp pairs at frame completion.
		struct Zone
		{
			const char* name = nullptr;
			u32 color = 0;
			u8 depth = 0;
		};

		Zone zones[vk::MAX_ZONES] = {};
		u32 zone_count = 0;
		u8 zone_depth = 0;

		// Match each end_zone() with its begin. ZONE_UNTIMED means there is no timestamp to close.
		u8 zone_stack[vk::MAX_ZONES] = {};
	};
}

namespace ember::gpu::vk
{
	inline constexpr u32 MAX_SWAPCHAIN_IMAGES = 8;
	inline constexpr u8 ZONE_UNTIMED = 0xFF;

	// Match the frame system's limit. Swapchains reserve an acquire semaphore for each slot.
	inline constexpr u32 MAX_IN_FLIGHT = 8;

	// Each graphics submit with pending uploads consumes a batch; end_frame() flushes any left.
	// Once these batches are spent, the frame cannot accept further uploads.
	inline constexpr u32 MAX_UPLOAD_BATCHES = 8;

	// A native queue and its timeline. The lock serializes submissions and their signal values.
	// Callers release it before waiting for GPU completion.
	struct QueueState
	{
		VkQueue handle = VK_NULL_HANDLE;
		u32 family = VK_QUEUE_FAMILY_IGNORED;
		VkSemaphore timeline = VK_NULL_HANDLE;

		// Whether the family can write timestamps. Transfer-only families often cannot, and a zone
		// on such a queue keeps its label but goes untimed.
		bool timestamps = false;

		// Last submission value assigned while holding lock.
		u64 submitted = 0;

		// Highest completed timeline value observed by a poll or successful wait.
		std::atomic<u64> completed{0};

		SpinMutex lock;
	};

	// Fixed after boot, except for each queue's submission state.
	struct Context
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice adapter = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

		// Route Vulkan and VMA CPU allocations through the Graphics heap. Pass the same callbacks
		// when releasing objects created with them.
		VkAllocationCallbacks callbacks = {};

		// Keep one entry per distinct native queue. Logical compute and transfer queues share the
		// graphics entry when the adapter has no dedicated family for them.
		QueueState queues[QUEUE_COUNT];
		u8 queue_index[QUEUE_COUNT] = {};
		u32 queue_count = 0;

		Caps caps;

		// Enabled shader stages used when translating resource states into barriers.
		VkPipelineStageFlags2 all_shader_stages = 0;

		u32 in_flight = 0;

		// Lists per frame slot for each logical queue, from GpuLimits, and their sum. Query ranges
		// go by slot, then by list index across the queue tables, then the finish list.
		u32 list_capacity[QUEUE_COUNT] = {};
		u32 list_total = 0;

		u32 max_swapchains = 0;
		bool debug_utils = false;
		bool headless = false;

		// Swapchain maintenance enables fences for safe presentation-resource retirement.
		bool present_fences = false;
	};

	// Native views and image read by recording. Keep them in the handle pool for direct lookup.
	struct TextureHot
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkImageView storage_view = VK_NULL_HANDLE;
	};

	// Allocation, subresource, and ownership details share the hot entry's index without adding to
	// the hot pool's stride.
	struct TextureCold
	{
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkExtent3D extent = {};
		TextureFormat api_format = TextureFormat::Undefined;
		TextureType type = TextureType::Texture2D;
		u32 mip_count = 1;
		u32 layer_count = 1;

		// Determines legal barriers, the post-upload state and sampled descriptor layout.
		TextureUsage usage = TextureUsage::None;

		// A non-null owner marks a backbuffer. The swapchain owns its image, and barriers update
		// the acquired image's tracked state through this link.
		SwapchainHandle swapchain = {};
		u32 backbuffer_index = 0;

		// Mips above zero get hidden pool entries for their storage views; mip zero uses the
		// texture's own entry. parent links each hidden entry to the texture that releases it.
		TextureHandle mip_storage[MAX_MIP_LEVELS] = {};
		TextureHandle parent = {};

		// Attachment views select one mip and layer, in mip-major order. The heap array is absent
		// when the main view already selects the only subresource.
		VkImageView* attachment_views = nullptr;
		u32 attachment_view_count = 0;
	};

	struct BufferHot
	{
		VkBuffer buffer = VK_NULL_HANDLE;
	};

	struct BufferCold
	{
		VmaAllocation allocation = VK_NULL_HANDLE;
		u64 size = 0;

		// Upload and Readback buffers stay mapped for their lifetime. DeviceLocal buffers do not.
		void* mapped = nullptr;

		BufferUsage usage = BufferUsage::None;
		MemoryLocation memory = MemoryLocation::DeviceLocal;
	};

	struct SamplerData
	{
		VkSampler sampler = VK_NULL_HANDLE;
	};

	// replace() may swap the pipeline while recording reads it; both use the atomic.
	struct PipelineData
	{
		std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};
	};

	// Owns a swapchain's native objects, semaphores, and backbuffer handles. The JobMutex
	// serializes acquisition and permits the owning fiber to park while waiting for an image.
	struct SwapchainData
	{
		jobs::JobMutex lock;

		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		WindowHandle window = {};

		VkSurfaceFormatKHR surface_format = {};
		VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
		PresentMode requested_mode = PresentMode::VSync;
		u32 preferred_image_count = 3;

		VkExtent2D extent = {};
		u32 image_count = 0;
		TextureHandle images[MAX_SWAPCHAIN_IMAGES] = {};

		// Each image owns its present semaphore and optional fence. pending marks an armed
		// fence. Without fences, semaphore reuse relies on reacquiring the image.
		VkSemaphore present_semaphores[MAX_SWAPCHAIN_IMAGES] = {};
		VkFence present_fences[MAX_SWAPCHAIN_IMAGES] = {};
		bool present_pending[MAX_SWAPCHAIN_IMAGES] = {};

		// Acquire signals the current slot's semaphore; the next graphics submit consumes it.
		// Waiting for the slot's previous frame makes the semaphore safe to reuse.
		VkSemaphore acquire_semaphores[MAX_IN_FLIGHT] = {};

		// The image acquired last and the frame that acquired it. The state that frame's lists
		// leave the image in travels with the slot's pending present.
		u32 acquired_image = 0;
		u64 acquired_frame = 0;

		bool needs_recreate = false;
	};

	// Retired storage waits here until frames that could use it have finished. Retirement stamps
	// never decrease, so entries ready for release form a prefix of the graveyard.
	struct Dead
	{
		enum class Kind : u8
		{
			Image,
			ImageView,
			Buffer,
			Sampler,
			Pipeline,
			Semaphore,
			Fence,
			Swapchain,
			Surface,

			// Reset descriptors before returning a retired pool index to the free ring.
			TextureSlot,
			BufferSlot,
			SamplerSlot,

			// CPU storage retained with a resource, such as its attachment-view array.
			Memory,
		};

		Kind kind = Kind::Image;

		// Stamp retirement with the latest frame begun, while holding resources_lock.
		u64 frame = 0;

		// Stores a native handle, pool index, or heap pointer according to kind.
		u64 handle = 0;
		VmaAllocation allocation = VK_NULL_HANDLE;
	};

	// An image acquired this frame, with the state its submitted lists have left it in so far.
	// end_frame() takes it the rest of the way to Present.
	struct PendingPresent
	{
		SwapchainHandle swapchain = {};
		u32 image = 0;
		TextureState state = TextureState::Undefined;
	};

	// Only the frame loop's fiber changes state. Closed publishes the queue values;
	// Complete follows their completion, zone resolution and Stage::Gpu end. Other
	// threads read the state but cannot reuse a slot while its finisher runs.
	enum class SlotState : u8
	{
		Idle,
		Open,
		Closed,
		Complete,
	};

	// Storage reused by successive frames once their GPU work has completed.
	struct Slot
	{
		// begin_command_list() claims these in order within the queue's table. Each list has a
		// private command pool so recording jobs do not share one.
		CommandList* lists[QUEUE_COUNT] = {};
		std::atomic<u32> opened[QUEUE_COUNT] = {};

		// end_frame() uses this list to prepare acquired images for presentation.
		CommandList finish;

		// Held until wait_oldest() returns it and clears this pointer. Recording, uploads,
		// acquisition and submission require Open; completion handling requires Closed.
		Frame* frame = nullptr;
		u64 frame_index = 0;
		std::atomic<SlotState> state{SlotState::Idle};

		// Last submission for this frame on each distinct queue, written before the slot closes.
		// Zero means it used no submission.
		u64 values[QUEUE_COUNT] = {};

		// acquire() appends under lock. end_frame() presents these images, and the next graphics
		// submit consumes their acquire semaphores.
		SpinMutex lock;
		PendingPresent presents[MAX_SWAPCHAINS] = {};
		u32 present_count = 0;
		VkSemaphore acquire_waits[MAX_SWAPCHAINS] = {};
		u32 acquire_wait_count = 0;

		// upload_lock serializes recording. Flush and submit hold the graphics queue lock;
		// that lock also protects upload_value, which covers all batches submitted so far.
		SpinMutex upload_lock;
		VkCommandPool upload_pool = VK_NULL_HANDLE;
		VkCommandBuffer upload_batches[MAX_UPLOAD_BATCHES] = {};
		u32 upload_batch = 0;
		bool upload_open = false;
		u64 upload_value = 0;

		// Space for (list_total + 1) * MAX_ZONES results, including end_frame()'s private list.
		ZoneTiming* zones = nullptr;
		u32 zone_count = 0;
	};

	inline bool is_open(const Slot& slot)
	{
		return slot.state.load(std::memory_order_acquire) == SlotState::Open;
	}

	// Shared by all pipelines: set 0 holds bindless arrays, set 1 holds constants.
	// Resource descriptors remain intact until retirement permits their indices to be reused.
	struct Heap
	{
		VkDescriptorSetLayout heap_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout constants_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkDescriptorSet constants = VK_NULL_HANDLE;

		// Fill unused descriptors and retired slots. Shutdown clears fallbacks_live before
		// releasing these resources so descriptor resets cannot reference destroyed objects.
		TextureHandle fallback_textures[(u32)TextureType::Count] = {};
		SamplerHandle fallback_sampler = {};
		BufferHandle fallback_buffer = {};
		VkImageView sampled_fallbacks[SAMPLED_ARRAY_COUNT] = {};
		VkImageView storage_fallbacks[STORAGE_ARRAY_COUNT] = {};
		VkSampler sampler_fallback = VK_NULL_HANDLE;
		VkBuffer buffer_fallback = VK_NULL_HANDLE;
		bool fallbacks_live = false;
	};

	// Cache-line alignment prevents false sharing between workers' private cursors.
	struct alignas(EMBER_CACHE_LINE) RingCursor
	{
		u64 cursor = 0;
		u64 end = 0;
	};

	// Workers claim chunks from next, then allocate without another shared-counter update.
	struct RingSlice
	{
		std::atomic<u64> next{0};
		RingCursor* cursors = nullptr;
		std::atomic<bool> starved{false};
	};

	// A mapped buffer divided among frame slots. The transient ring also has a buffer-pool entry
	// for shader access; the staging ring is used directly by copy commands.
	struct Ring
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		BufferHandle handle = {};
		u8* cpu = nullptr;
		u64 slice_bytes = 0;
		RingSlice* slices = nullptr;
		u32 worker_count = 0;
		const char* name = "ring";
	};

	// A null cpu pointer means allocation failed.
	struct RingSpan
	{
		u8* cpu = nullptr;
		u64 offset = 0;
	};

	struct Backend
	{
		Context ctx;
		GpuDef def;

		Slot* slots = nullptr;

		// Updated under resources_lock so frame starts and resource retirements receive a
		// consistent order. bury() uses it to choose a retirement stamp.
		u64 latest_frame = 0;

		SpinMutex resources_lock;
		Pool<TextureHot, Texture> textures;
		TextureCold* texture_cold = nullptr;
		Pool<BufferHot, Buffer> buffers;
		BufferCold* buffer_cold = nullptr;
		Pool<SamplerData, Sampler> samplers;
		Pool<PipelineData, GraphicsPipeline> graphics_pipelines;
		Pool<PipelineData, ComputePipeline> compute_pipelines;
		Pool<SwapchainData, Swapchain> swapchains;

		Heap heap;
		Ring transient;
		Ring staging;

		// Reservations include unused worker chunk tails; peaks help size ring budgets.
		u64 last_transient_used = 0;
		u64 last_staging_used = 0;
		u64 peak_transient_used = 0;
		u64 peak_staging_used = 0;

		// Protected by resources_lock. The prefix before graveyard_head has already been released.
		Vector<Dead> graveyard{memory::Tag::Graphics};
		u32 graveyard_head = 0;

		// Query pairs are divided by slot and list. Null if timestamps are unsupported or
		// allocation of the query pool failed.
		VkQueryPool timestamps = VK_NULL_HANDLE;

		// Once a Vulkan call reports device loss, keep it visible to later calls and waits.
		std::atomic<bool> lost{false};
	};

	// Null before initialization and after shutdown.
	Backend* backend();

	bool boot(Backend& backend, const GpuDef& def);
	void teardown(Backend& backend);
	void note_result(Backend& backend, VkResult result);
	void set_name(const Context& ctx, VkObjectType type, u64 handle, const char* name);

	QueueState& queue_for(Backend& backend, Queue queue);

	Slot& slot_of(Backend& backend, const Frame& frame);

	void raise_completed(QueueState& queue, u64 value);

	// Read each queue's timeline into its completed counter. Any thread.
	void poll_queues(Backend& backend);

	// Resolves zones and ends Stage::Gpu for completed queue values. Only the frame loop's fiber.
	void finish_frames(Backend& backend);

	// Descriptor replacement requires no frame open, so recording cannot race the update.
	bool any_frame_open(const Backend& backend);

	// Requires no frame open and the frame loop's fiber. Parks until all frames' GPU work ends.
	void wait_frames(Backend& backend);

	// Any thread may wait for a timeline value. Completed values return immediately; pending ones
	// use run_blocking(). A lost device also ends the wait without claiming completion.
	void wait_value(Backend& backend, QueueState& queue, u64 value);

	// Parks scheduler fibers through the I/O lane. External callers and refused I/O jobs run
	// the blocking call directly.
	void run_blocking(jobs::JobFn fn, void* data, const char* name);

	// Append a retirement stamped with the newest frame. The caller must hold resources_lock.
	void bury(Backend& backend, Dead dead);

	// Release entries older than every incomplete frame. everything bypasses that check and
	// requires GPU use to have stopped. Hold resources_lock for pool reuse and bookkeeping, then
	// release it before destroying native objects in batches.
	void drain(Backend& backend, bool everything);

	// Give swapchain-owned images texture handles for recording. Releasing a backbuffer retires its
	// view and pool entry without freeing the image owned by the swapchain.
	TextureHandle wrap_backbuffer(Backend& backend, VkImage image, VkImageView view,
	    VkExtent2D extent, TextureFormat format, TextureUsage usage, SwapchainHandle swapchain,
	    u32 image_index);
	void release_backbuffer(Backend& backend, TextureHandle handle);

	void record_finish(Backend& backend, Slot& slot, VkCommandBuffer commands);
	u32 present_signals(Backend& backend, Slot& slot, VkSemaphoreSubmitInfo* signals);
	void present(Backend& backend, Slot& slot);

	// Shared by recording and end_frame() so both produce the same barriers.
	struct StateInfo
	{
		VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
		VkAccessFlags2 access = VK_ACCESS_2_NONE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	StateInfo state_info(TextureState state, VkPipelineStageFlags2 shader_stages);

	// Storage-capable textures use GENERAL for both sampled and storage descriptors.
	// Other sampled textures use the read-only layout.
	VkImageLayout read_layout_of(TextureUsage usage);

	// ShaderRead must match the texture's descriptors; other states have fixed layouts.
	VkImageLayout layout_of(const TextureCold& cold, TextureState state);

	// Restricts barrier stages to those supported by the queue; transfer has no shader stages.
	VkPipelineStageFlags2 shader_stages_of(const Context& ctx, Queue queue);

	// Chooses ShaderRead, an attachment state or CopyDst according to usage. An attachment-only
	// image cannot use SHADER_READ_ONLY_OPTIMAL.
	TextureState uploaded_state(TextureUsage usage);

	// Buffers are shared by all queue families and need no ownership transfers. Images remain
	// exclusive and require explicit transfers.
	struct BufferSharing
	{
		VkSharingMode mode = VK_SHARING_MODE_EXCLUSIVE;
		u32 count = 0;
		u32 families[QUEUE_COUNT] = {};
	};

	BufferSharing buffer_sharing(const Context& ctx);

	// Build the layouts and sets before creating resources that write descriptors. Fallbacks need
	// resource pools, and constant descriptors need the transient ring. Serialize descriptor writes
	// and resets with resources_lock once the backend is in use.
	bool heap_boot(Backend& backend);
	bool heap_fallbacks(Backend& backend);
	void heap_bind_constants(Backend& backend);
	void heap_release_fallbacks(Backend& backend);
	void heap_shutdown(Backend& backend);
	void heap_write_sampled(Backend& backend, u32 slot, VkImageView view, VkImageLayout layout,
	    TextureType type);
	void heap_write_storage(Backend& backend, u32 slot, VkImageView view, TextureType type);
	void heap_write_sampler(Backend& backend, u32 slot, VkSampler sampler);
	void heap_write_buffer(Backend& backend, u32 slot, VkBuffer buffer, u64 size);
	void heap_reset_texture(Backend& backend, u32 slot, TextureType type);
	void heap_reset_sampler(Backend& backend, u32 slot);
	void heap_reset_buffer(Backend& backend, u32 slot);

	// Create the rings at boot, reset slices only after their frame slot completes, and allocate
	// within a slice while that frame is open. Upload batches use the slot's separate lock.
	bool rings_boot(Backend& backend);
	void rings_shutdown(Backend& backend);
	void ring_reset(Ring& ring, u32 slot);
	RingSpan ring_allocate(Ring& ring, u32 slot, u64 size, u64 alignment);
	u64 ring_used(const Ring& ring, u32 slot);

	// Returns the closed upload batch, or null if empty. Hold the graphics queue lock through
	// submission to preserve order. Takes upload_lock internally.
	VkCommandBuffer flush_uploads(Slot& slot);
}
