#pragma once

// State shared by the Vulkan sources. gpu::initialize() creates one Backend on the Graphics heap,
// and gpu::shutdown() releases it. Static storage only holds the pointer.
//
// Device configuration is fixed after boot. Queue counters keep changing as work is submitted and
// completed, using their own locks and atomics. Each frame in flight has a slot for its command
// lists, uploads, acquisitions, and last submission on each queue. A slot can be reused once all of
// those submissions have completed.
//
// Resource creation and destruction change the pools under resources_lock. Recording looks up
// handles without that lock, so callers must keep resources alive while recording uses them.
// Destruction retires the native objects and descriptor slots until earlier frames finish.
//
// Descriptors use the resource's pool index. Constants and other transient data come from one
// mapped ring; uploads copy through another. Both rings have a slice for each frame slot, so
// reclaiming their storage follows the same completion rule as the command lists.

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

	// One recording writer owns the command buffer, its private pools, and cached binding state
	// between begin_command_list() and end_command_list(). The slot keeps them for reuse.
	struct CommandList
	{
		// The buffer recording now: the entry of buffers[] for the queue begin_command_list()
		// named, which is the only queue the list may be submitted to.
		VkCommandBuffer commands = VK_NULL_HANDLE;
		Queue queue = Queue::Graphics;

		// A pool and primary buffer for each distinct native queue, indexed like Context::queues,
		// because a buffer may only go to a queue of the family its pool was made for.
		VkCommandPool pools[vk::QUEUE_COUNT] = {};
		VkCommandBuffer buffers[vk::QUEUE_COUNT] = {};

		// Together these locate the list's timestamp queries as well as its frame slot.
		u32 slot = 0;
		u32 index = 0;

		// Records which frame opened the list and which operations are legal next. Ending permits
		// submission; submission prevents the list from being used again until its slot resets.
		u64 frame_index = 0;
		bool open = false;
		bool ended = false;
		bool submitted = false;
		bool inside_pass = false;

		// The name supplied at begin_command_list(), used to label the whole recording.
		const char* name = nullptr;

		// Remember the last bindings so repeated requests need no Vulkan call. Draws check that a
		// pipeline is bound, and indexed draws also check for an index buffer.
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

		// The state this list last left each swapchain's acquired image in. submit() folds these
		// into the slot's pending presents in submission order, which is the order the GPU sees;
		// recording order would mislead end_frame() whenever lists are submitted out of it. A
		// frame acquires one image per swapchain, so one entry per swapchain is enough.
		struct BackbufferUse
		{
			SwapchainHandle swapchain = {};
			u32 image = 0;
			TextureState state = TextureState::Undefined;
		};

		BackbufferUse backbuffers[vk::MAX_SWAPCHAINS] = {};
		u32 backbuffer_count = 0;

		// Keep labels and nesting on the CPU; the query pool only needs timestamp pairs. Completion
		// combines them into the frame's timing results.
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

	// Device objects and configuration established at boot. These stay fixed while the backend
	// runs, except for the submission state kept inside queues.
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
		u32 max_lists = 0;
		u32 max_swapchains = 0;
		bool debug_utils = false;
		bool headless = false;

		// VK_EXT_swapchain_maintenance1 is enabled, so each present can signal a fence that says
		// when the presentation engine is done with the image and its semaphore.
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
		VkFormat format = VK_FORMAT_UNDEFINED;
		TextureFormat api_format = TextureFormat::Undefined;
		TextureType type = TextureType::Texture2D;
		u32 mip_count = 1;
		u32 layer_count = 1;

		// The usage the texture was created with. Barriers check it, and an upload chooses the
		// state it leaves the texture in by it.
		TextureUsage usage = TextureUsage::None;

		// ShaderRead must agree with the sampled descriptor's layout. Storage-capable textures use
		// GENERAL for both; other textures use SHADER_READ_ONLY_OPTIMAL.
		VkImageLayout read_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

	// Recording only needs the native buffer; allocation details stay in BufferCold.
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

	// Pipelines all share the bindless heap's layout, so only the compiled pipeline is needed.
	struct PipelineData
	{
		VkPipeline pipeline = VK_NULL_HANDLE;
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

		// Present waits on the semaphore belonging to its image. The fence beside it, when the
		// device has present fences, signals once presentation is done with both, and the
		// pending flag says the fence is armed. Without fences, reacquiring the image is the only
		// sign that presentation has consumed the previous wait.
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

	// Where a frame slot is in its life. begin_frame() makes it Open after waiting out Closed and
	// Finishing; end_frame() makes it Closed once its queue values are written. Whoever sees the
	// values pass claims Finishing alone, resolves the zones, ends Stage::Gpu and publishes
	// Complete, and a waiter that lost the claim spins until it sees Complete, so nothing reuses
	// the slot under the finisher. Idle is a slot no frame has used yet.
	enum class SlotState : u8
	{
		Idle,
		Open,
		Closed,
		Finishing,
		Complete,
	};

	// Storage reused by successive frames once their GPU work has completed.
	struct Slot
	{
		// begin_command_list() claims these in order. Each has a private command pool so recording
		// jobs do not share a pool.
		CommandList* lists = nullptr;
		std::atomic<u32> opened{0};

		// end_frame() uses this list to prepare acquired images for presentation.
		CommandList finish;

		// Keep the frame until its slot is reused. The state says what the slot allows: recording,
		// uploads, acquisition and submission while Open, completion handling once Closed.
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

		// Serialize upload recording from any thread. The next graphics submit places the current
		// batch before its caller's command lists.
		SpinMutex upload_lock;
		VkCommandPool upload_pool = VK_NULL_HANDLE;
		VkCommandBuffer upload_batches[MAX_UPLOAD_BATCHES] = {};
		u32 upload_batch = 0;
		bool upload_open = false;

		// Space for (max_lists + 1) * MAX_ZONES results, including end_frame()'s private list.
		ZoneTiming* zones = nullptr;
		u32 zone_count = 0;
	};

	// True while the slot's frame may still record, upload, acquire and submit.
	inline bool is_open(const Slot& slot)
	{
		return slot.state.load(std::memory_order_acquire) == SlotState::Open;
	}

	// Set 0 holds the bindless arrays and set 1 the constant slots. All pipelines use this layout.
	// A resource's descriptor uses its pool index and stays intact after destruction until the
	// graveyard can reset it, keeping earlier frames' descriptors valid.
	struct Heap
	{
		VkDescriptorSetLayout heap_layout = VK_NULL_HANDLE;
		VkDescriptorSetLayout constants_layout = VK_NULL_HANDLE;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkDescriptorSet constants = VK_NULL_HANDLE;

		// One fallback texture per type, plus a sampler and buffer. They are created before caller
		// resources, and their native objects fill unused descriptors and retired slots. Shutdown
		// clears fallbacks_live before releasing them so later resets do not use dying objects.
		TextureHandle fallback_textures[(u32)TextureType::Count] = {};
		SamplerHandle fallback_sampler = {};
		BufferHandle fallback_buffer = {};
		VkImageView sampled_fallbacks[SAMPLED_ARRAY_COUNT] = {};
		VkImageView storage_fallbacks[STORAGE_ARRAY_COUNT] = {};
		VkSampler sampler_fallback = VK_NULL_HANDLE;
		VkBuffer buffer_fallback = VK_NULL_HANDLE;
		bool fallbacks_live = false;
	};

	// Each worker advances its own cursor. Cache-line alignment keeps neighbouring workers from
	// writing the same cache line while they allocate.
	struct alignas(EMBER_CACHE_LINE) RingCursor
	{
		u64 cursor = 0;
		u64 end = 0;
	};

	// A frame slot's slice of the ring. Workers claim chunks from next, then allocate within their
	// own chunk without updating a shared counter on every request.
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

	// The writable mapping and buffer offset for one ring allocation. An empty span means failure.
	struct RingSpan
	{
		u8* cpu = nullptr;
		u64 offset = 0;
	};

	struct Backend
	{
		Context ctx;
		GpuDef def;

		// A frame uses slots[frame.index % in_flight] after the prior owner has finished.
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

		// Ring space reserved by the last frame that ended, including unused worker chunk space.
		u64 last_transient_used = 0;
		u64 last_staging_used = 0;

		// Protected by resources_lock. The prefix before graveyard_head has already been released.
		Vector<Dead> graveyard{memory::Tag::Graphics};
		u32 graveyard_head = 0;

		// Query pairs are divided by slot and list. Null if timestamps are unsupported or
		// allocation of the query pool failed.
		VkQueryPool timestamps = VK_NULL_HANDLE;

		// Once a Vulkan call reports device loss, keep it visible to later calls and waits.
		std::atomic<bool> lost{false};
	};

	// Returns the backend while initialization or normal use owns it; null before setup and after
	// shutdown has released it.
	Backend* backend();

	bool boot(Backend& backend, const GpuDef& def);
	void teardown(Backend& backend);
	void note_result(Backend& backend, VkResult result);
	void set_name(const Context& ctx, VkObjectType type, u64 handle, const char* name);

	QueueState& queue_for(Backend& backend, Queue queue);
	Slot& slot_of(Backend& backend, u64 frame_index);
	void raise_completed(QueueState& queue, u64 value);

	// Poll each queue's timeline, then resolve closed frames whose submission values have passed
	// and end their Stage::Gpu.
	void poll(Backend& backend);

	// Any thread may wait for a timeline value. Completed values return immediately; pending ones
	// use run_blocking(). A lost device also ends the wait without claiming completion.
	void wait_value(Backend& backend, QueueState& queue, u64 value);

	// Prefer the IO lane for driver calls that may block, parking a job or main's fiber until done.
	// External threads run directly. If the IO lane refuses the job, the caller runs it directly
	// too.
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

	// end_frame() transitions acquired images, signals their present semaphores, and presents them
	// through these helpers.
	void record_finish(Backend& backend, Slot& slot, VkCommandBuffer commands);
	u32 present_signals(Backend& backend, Slot& slot, VkSemaphoreSubmitInfo* signals);
	void present(Backend& backend, Slot& slot);

	// The Vulkan stage, access, and layout masks behind a texture state. Recording and end_frame()
	// use the same translation so their barriers agree.
	struct StateInfo
	{
		VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
		VkAccessFlags2 access = VK_ACCESS_2_NONE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	StateInfo state_info(TextureState state, VkPipelineStageFlags2 shader_stages);

	// The image layout a state means for one texture. ShaderRead follows the layout written in
	// the texture's descriptors; every other state has a single layout.
	VkImageLayout layout_of(const TextureCold& cold, TextureState state);

	// The shader stages a queue's commands may name in a barrier. A compute queue has only the
	// compute stage and a transfer queue none; naming a stage the family lacks is invalid.
	VkPipelineStageFlags2 shader_stages_of(const Context& ctx, Queue queue);

	// The state an upload leaves a texture in, chosen by usage: ShaderRead where a shader can read
	// it, otherwise its attachment state, otherwise CopyDst. SHADER_READ_ONLY_OPTIMAL is only
	// legal on an image with sampled usage, so an attachment-only texture must not end there.
	TextureState uploaded_state(TextureUsage usage);

	// Buffers are visible to every distinct queue family, so compute and transfer lists can use
	// them without ownership transfers. Images stay exclusive, which keeps their compression.
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

	// Close the open upload batch and return it to precede the submit's lists, or null if empty.
	// This helper takes upload_lock itself.
	VkCommandBuffer flush_uploads(Slot& slot);
}
