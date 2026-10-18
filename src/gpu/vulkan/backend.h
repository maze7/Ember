#pragma once

// The Vulkan backend's state and the functions its sources share. One Backend exists between
// gpu::initialize() and gpu::shutdown(), on the heap under Tag::Graphics so that no static holds
// memory. Three kinds of state live in it, with three timelines:
//
// 		Context		written at boot and read-only afterwards, so it needs no lock.
//
// 		Slots		one per frame in flight, written by the loop's bookends and by the jobs
// 					recording for the frame that holds the slot, which the frame's own order keeps
// 					apart.
//
// 		Pools		written by creation and destruction from any thread under resources_lock and
// 					read lock free through handles. The graveyard beside them holds what has been
// 					destroyed until every frame that could have read it has completed.
//
// A frame's GPU work is complete when every queue's timeline has passed the frame's last submission
// on it. Those values live in the slot; the queue's completed counters are what the device has seen
// the GPU reach, written by whoever polls and read by anyone.

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
		// GPU zones one list may time per frame. Deeper nesting still needs labels, untimes.
		inline constexpr u32 MAX_ZONES = 64;
	}

	// What one list records through: the native buffer, the pool only it allocates from, and the
	// shadow state the lazy binds compare against. One writer from begin to end.
	struct CommandList
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE;

		// Its slot and its place in the slot's table, which is also its timestamp slice.
		u32 slot = 0;
		u32 index = 0;

		// The frame that opened it. Open from begin to end, ended from end to submit, submitted
		// from then until the slot is reused.
		u64 frame_index = 0;
		bool open = false;
		bool ended = false;
		bool submitted = false;
		bool inside_pass = false;

		// The name begin_command_list() was given, a debug label around the whole list.
		const char* name = nullptr;

		// Redundancy filters: binding the bound pipeline again is free to skip.
		GraphicsPipelineHandle graphics_pipeline = {};
		ComputePipelineHandle compute_pipeline = {};

		VkBuffer index_buffer = VK_NULL_HANDLE;
		u64 index_offset = 0;
		VkIndexType index_type = VK_INDEX_TYPE_UINT32;
		bool index_dirty = false;

		// Constants serve both bind points; each flushes on its own schedule.
		u32 constant_offsets[CONSTANT_SLOTS] = {};
		bool constants_dirty_graphics = false;
		bool constants_dirty_compute = false;

		// Zones recorded by this list: names and colors stay CPU side, the query pool holds only
		// the ticks. Resolved when the frame completes.
		struct Zone
		{
			const char* name = nullptr;
			u32 color = 0;
			u8 depth = 0;
		};

		Zone zones[vk::MAX_ZONES] = {};
		u32 zone_count = 0;
		u8 zone_depth = 0;

		// Open zone indices by depth; ZONE_UNTIMED marks one that wrote no begin.
		u8 zone_stack[vk::MAX_ZONES] = {};
	};
}

namespace ember::gpu::vk
{
	inline constexpr u32 QUEUE_COUNT = (u32)Queue::Count;
	inline constexpr u32 MAX_SWAPCHAINS = 8;
	inline constexpr u32 MAX_SWAPCHAIN_IMAGES = 8;
	inline constexpr u8 ZONE_UNTIMED = 0xFF;

	// frames.cpp allows this many in flight; the swapchains keep one acquire semaphore per slot.
	inline constexpr u32 MAX_IN_FLIGHT = 8;

	// One VkQueue and its timeline. A submit takes the lock, hands out the next value and signals
	// it. Nothing waits under the lock.
	struct QueueState
	{
		VkQueue handle = VK_NULL_HANDLE;
		u32 family = VK_QUEUE_FAMILY_IGNORED;
		VkSemaphore timeline = VK_NULL_HANDLE;

		// The last value handed to a submit, under the lock.
		u64 submitted = 0;

		// The highest value the device has seen the GPU reach.
		std::atomic<u64> completed{0};

		SpinMutex lock;
	};

	// Everything made once at boot and read afterwards. Only boot.cpp writes it.
	struct Context
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice adapter = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

		// Every create and destroy passes these, so Vulkan's and VMA's CPU memory comes from the
		// heap under Tag::Graphics.
		VkAllocationCallbacks callbacks = {};

		// The distinct queues, and the one each Queue maps to: an adapter without a compute or
		// transfer family of its own maps those onto the graphics entry.
		QueueState queues[QUEUE_COUNT];
		u8 queue_index[QUEUE_COUNT] = {};
		u32 queue_count = 0;

		Caps caps;

		// Every enabled stage that can read a resource, for the barrier tables.
		VkPipelineStageFlags2 all_shader_stages = 0;

		u32 in_flight = 0;
		u32 max_lists = 0;
		u32 max_swapchains = 0;
		bool debug_utils = false;
		bool headless = false;
	};

	// A texture's hot half: what recording reads. Lives in the pool.
	struct TextureHot
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkImageView storage_view = VK_NULL_HANDLE;
	};

	// The cold half, beside the pool by index: what creation, destruction and the rare lookup read.
	struct TextureCold
	{
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkExtent3D extent = {};
		VkFormat format = VK_FORMAT_UNDEFINED;
		TextureFormat api_format = TextureFormat::Undefined;
		TextureType type = TextureType::Texture2D;
		u32 mip_count = 1;
		u32 layer_count = 1;

		// A backbuffer: the swapchain owns the image, and a barrier on it tells the swapchain
		// which state the image is in.
		SwapchainHandle swapchain = {};
		u32 backbuffer_index = 0;

		// Hidden entries for mips 1 and up with storage views; mip 0 is the texture's own slot.
		// An entry names its owner in [parent] and dies with it.
		TextureHandle mip_storage[MAX_MIP_LEVELS] = {};
		TextureHandle parent = {};

		// Single slice views for a target with more than one subresource, mip major, from the
		// heap. Null when the whole view serves.
		VkImageView* attachment_views = nullptr;
		u32 attachment_view_count = 0;
	};

	// One swapchain: the native objects, its semaphores and its backbuffer pool entries. The
	// JobMutex orders acquires from different jobs, and may be held across the park an acquire
	// makes while the display is behind.
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

		// One per image, signalled by the frame's last submit and waited by its present. Reuse is
		// safe because re-acquiring an image means the presentation engine consumed its wait.
		VkSemaphore present_semaphores[MAX_SWAPCHAIN_IMAGES] = {};

		// One per frame slot, signalled by the acquire and waited by the frame's first graphics
		// submit. Reuse is safe because the slot's previous frame has completed.
		VkSemaphore acquire_semaphores[MAX_IN_FLIGHT] = {};

		// The image this frame acquired, and the state barriers have moved it to since.
		u32 acquired_image = 0;
		u64 acquired_frame = 0;
		TextureState acquired_state = TextureState::Undefined;

		bool needs_recreate = false;
	};

	// Something destroyed, waiting for every frame that could read it to complete. Entries are
	// pushed with non-decreasing frame stamps, so the releasable ones are a prefix.
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
			Swapchain,
			Surface,

			// A retired texture pool slot going back on the ring.
			TextureSlot,

			// Heap memory of a dead resource, such as a texture's attachment views.
			Memory,
		};

		Kind kind = Kind::Image;

		// The newest frame begun when this died.
		u64 frame = 0;

		// The Vulkan handle's bits, the pool index, or the heap pointer.
		u64 handle = 0;
		VmaAllocation allocation = VK_NULL_HANDLE;
	};

	struct PendingPresent
	{
		SwapchainHandle swapchain = {};
		u32 image = 0;
	};

	// One frame in flight: its command lists, what it acquired and what it submitted.
	struct Slot
	{
		// max_lists of them, each with a pool of its own, claimed in order by begin_command_list.
		CommandList* lists = nullptr;
		std::atomic<u32> opened{0};

		// end_frame's own list: the backbuffer transitions.
		CommandList finish;

		// The frame holding the slot, null when free, and whether end_frame() has closed it: a
		// closed slot takes no more lists, submits or acquires, and completes once its values pass.
		Frame* frame = nullptr;
		u64 frame_index = 0;
		bool closed = false;

		// The frame's last submission on each distinct queue; zero for one it never used.
		u64 values[QUEUE_COUNT] = {};

		// Stage::Gpu ended: the GPU has finished the frame and its zones are resolved.
		std::atomic<bool> complete{true};

		// What acquire() recorded this frame, under the lock: the images to present at end_frame,
		// and the acquire semaphores the next graphics submit waits on.
		SpinMutex lock;
		PendingPresent presents[MAX_SWAPCHAINS] = {};
		u32 present_count = 0;
		VkSemaphore acquire_waits[MAX_SWAPCHAINS] = {};
		u32 acquire_wait_count = 0;

		// The zones of the frame, (max_lists + 1) * MAX_ZONES of them, resolved at completion.
		ZoneTiming* zones = nullptr;
		u32 zone_count = 0;
	};

	struct Backend
	{
		Context ctx;
		GpuDef def;

		// in_flight slots; a frame's is frame.index % in_flight.
		Slot* slots = nullptr;

		// The newest frame begun, written under resources_lock so that a destroy's stamp and a
		// frame's begin order against each other.
		u64 latest_frame = 0;

		SpinMutex resources_lock;
		Pool<TextureHot, Texture> textures;
		TextureCold* texture_cold = nullptr;
		Pool<SwapchainData, Swapchain> swapchains;

		// Under resources_lock. Entries from graveyard_head on are pending.
		Vector<Dead> graveyard{memory::Tag::Graphics};
		u32 graveyard_head = 0;

		// Timestamps, sliced per slot and list; null without Caps::timestamps.
		VkQueryPool timestamps = VK_NULL_HANDLE;

		// VK_ERROR_DEVICE_LOST was seen. Sticky.
		std::atomic<bool> lost{false};
	};

	// The one backend, null outside initialize() and shutdown().
	Backend* backend();

	bool boot(Backend& backend, const GpuDef& def);
	void teardown(Backend& backend);
	void note_result(Backend& backend, VkResult result);
	void set_name(const Context& ctx, VkObjectType type, u64 handle, const char* name);

	QueueState& queue_for(Backend& backend, Queue queue);
	Slot& slot_of(Backend& backend, u64 frame_index);
	void raise_completed(QueueState& queue, u64 value);

	// Reads every timeline and ends Stage::Gpu for the frames that have completed.
	void poll(Backend& backend);

	// Parks the caller until the queue's timeline reaches value. Any thread.
	void wait_value(Backend& backend, QueueState& queue, u64 value);

	// Runs a call that blocks on the driver: on an IO thread with the fiber parked when the caller
	// is a job or main, directly on a thread outside the scheduler.
	void run_blocking(jobs::JobFn fn, void* data, const char* name);

	// Puts a dead thing in the graveyard, stamped with the newest frame. The caller holds
	// resources_block.
	void bury(Backend& backend, Dead dead);

	// Releases what no incomplete frame can name, or everything after an idle. Takes the lock.
	void drain(Backend& backend, bool everything);

	// Backbuffers are texture pool entries the swapchain owns.
	TextureHandle wrap_backbuffer(Backend& backend, VkImage image, VkImageView view,
	    VkExtent2D extent, TextureFormat format, SwapchainHandle swapchain, u32 image_index);
	void release_backbuffer(Backend& backend, TextureHandle handle);

	// What end_frame needs: the backbuffer transitions, the present semaphores its submit signals,
	// and the presents themselves.
	void record_finish(Backend& backend, Slot& slot, VkCommandBuffer commands);
	u32 present_signals(Backend& backend, Slot& slot, VkSemaphoreSubmitInfo* signals);
	void present(Backend& backend, Slot& slot);

	// The stage, access and layout a state names, for barriers here and in the swapchain's finish.
	struct StateInfo
	{
		VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
		VkAccessFlags2 access = VK_ACCESS_2_NONE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	StateInfo state_info(TextureState state, VkPipelineStageFlags2 shader_stages);
}
