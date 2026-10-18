// The frame loop's side of the device: the slots, the command lists they hand out, the submits,
// the waits on the timelines and the graveyard. A frame's slot is claimed at begin_frame(), closed
// at end_frame() and complete once every queue has passed the frame's last submission on it, which
// poll() looks for and wait_value() parks for.

#include <ember/gpu/gpu.h>

#include <ember/core/frame.h>
#include <ember/core/jobs.h>
#include <ember/core/profile.h>
#include <ember/memory/heap.h>
#include <gpu/vulkan/backend.h>

#if defined(EMBER_HAS_PLATFORM)
	#include <platform/wsi.h>
#endif

namespace ember::gpu
{
	namespace
	{
		// One vkQueueSubmit2 carries this many lists; a bigger submit goes in chunks under the
		// lock.
		constexpr u32 LISTS_PER_SUBMIT = 32;
		constexpr u32 MAX_WAITS = 16;

		struct TimelineWait
		{
			VkDevice device = VK_NULL_HANDLE;
			VkSemaphore semaphore = VK_NULL_HANDLE;
			u64 value = 0;
			VkResult result = VK_SUCCESS;
		};

		void timeline_wait_job(void* data)
		{
			TimelineWait& wait = *(TimelineWait*)data;
			const VkSemaphoreWaitInfo info{
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			    .semaphoreCount = 1,
			    .pSemaphores = &wait.semaphore,
			    .pValues = &wait.value,
			};
			wait.result = vkWaitSemaphores(wait.device, &info, UINT64_MAX);
		}

		struct IdleWait
		{
			VkDevice device = VK_NULL_HANDLE;
			VkResult result = VK_SUCCESS;
		};

		void idle_job(void* data)
		{
			IdleWait& wait = *(IdleWait*)data;
			wait.result = vkDeviceWaitIdle(wait.device);
		}

		// Back to the state a list has between frames; the natives and its place stay.
		void reset_list(CommandList& list)
		{
			list = CommandList{.pool = list.pool,
			    .commands = list.commands,
			    .slot = list.slot,
			    .index = list.index};
		}

		u32 slot_index(const vk::Backend& b, const vk::Slot& slot)
		{
			return (u32)(&slot - b.slots);
		}

		bool values_passed(const vk::Backend& b, const vk::Slot& slot)
		{
			for (u32 i = 0; i < b.ctx.queue_count; ++i)
			{
				if (slot.values[i] > b.ctx.queues[i].completed.load(std::memory_order_acquire))
				{
					return false;
				}
			}

			return true;
		}

		// Reads the ticks the frame's lists wrote into the slot's zone table, list order.
		void resolve_zones(vk::Backend& b, vk::Slot& slot)
		{
			slot.zone_count = 0;
			if (b.timestamps == VK_NULL_HANDLE)
			{
				return;
			}

			const u32 opened = slot.opened.load(std::memory_order_acquire);
			const u32 count = opened < b.ctx.max_lists ? opened : b.ctx.max_lists;
			const f32 period = b.ctx.caps.timestamp_period_ns;

			for (u32 i = 0; i <= count; ++i)
			{
				const CommandList& list = i < count ? slot.lists[i] : slot.finish;
				if (list.zone_count == 0)
				{
					continue;
				}

				const u32 base = ((slot_index(b, slot) * (b.ctx.max_lists + 1)) + list.index) *
				                 vk::MAX_ZONES * 2;
				u64 ticks[vk::MAX_ZONES * 2];

				if (vkGetQueryPoolResults(b.ctx.device, b.timestamps, base, list.zone_count * 2,
				        sizeof(u64) * list.zone_count * 2, ticks, sizeof(u64),
				        VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
				{
					continue;
				}

				for (u32 zone = 0; zone < list.zone_count; ++zone)
				{
					slot.zones[slot.zone_count++] = {
					    .name = list.zones[zone].name,
					    .color = list.zones[zone].color,
					    .depth = list.zones[zone].depth,
					    .duration_ms =
					        (f32)(ticks[zone * 2 + 1] - ticks[zone * 2]) * period / 1.0e6f,
					};
				}
			}
		}

		// The frame's GPU work is done: once, whoever notices first.
		void complete_slot(vk::Backend& b, vk::Slot& slot)
		{
			if (slot.complete.exchange(true, std::memory_order_acq_rel))
			{
				return;
			}

			resolve_zones(b, slot);
			frames::end_stage(*slot.frame, Stage::Gpu);
		}

		void wait_slot(vk::Backend& b, vk::Slot& slot)
		{
			for (u32 i = 0; i < b.ctx.queue_count; ++i)
			{
				vk::wait_value(b, b.ctx.queues[i], slot.values[i]);
			}

			complete_slot(b, slot);
		}

		// Destroys one dead thing. The caller holds resources_lock; every call here is short.
		void release(vk::Backend& b, const vk::Dead& dead)
		{
			const VkAllocationCallbacks* callbacks = &b.ctx.callbacks;

			switch (dead.kind)
			{
				case vk::Dead::Kind::Image:
					vmaDestroyImage(b.ctx.allocator, (VkImage)dead.handle, dead.allocation);
					break;
				case vk::Dead::Kind::ImageView:
					vkDestroyImageView(b.ctx.device, (VkImageView)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Buffer:
					vmaDestroyBuffer(b.ctx.allocator, (VkBuffer)dead.handle, dead.allocation);
					break;
				case vk::Dead::Kind::Sampler:
					vkDestroySampler(b.ctx.device, (VkSampler)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Pipeline:
					vkDestroyPipeline(b.ctx.device, (VkPipeline)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Semaphore:
					vkDestroySemaphore(b.ctx.device, (VkSemaphore)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Swapchain:
					vkDestroySwapchainKHR(b.ctx.device, (VkSwapchainKHR)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Surface:
					// The platform made the surface without our callbacks, so it unmakes it.
#if defined(EMBER_HAS_PLATFORM)
					platform::vulkan::destroy_surface(b.ctx.instance, (VkSurfaceKHR)dead.handle);
#else
					vkDestroySurfaceKHR(b.ctx.instance, (VkSurfaceKHR)dead.handle, nullptr);
#endif
					break;
				case vk::Dead::Kind::TextureSlot:
					b.textures.release((u32)dead.handle);
					break;
				case vk::Dead::Kind::Memory:
					memory::free((void*)dead.handle);
					break;
			}
		}
	}

	namespace vk
	{
		QueueState& queue_for(Backend& b, Queue queue)
		{
			EMBER_ASSERT(queue < Queue::Count);
			return b.ctx.queues[b.ctx.queue_index[(u32)queue]];
		}

		Slot& slot_of(Backend& b, u64 frame_index)
		{
			Slot& slot = b.slots[frame_index % b.ctx.in_flight];
			EMBER_ASSERT_MSG(slot.frame_index == frame_index, "a frame the device never began");
			return slot;
		}

		void raise_completed(QueueState& queue, u64 value)
		{
			u64 seen = queue.completed.load(std::memory_order_relaxed);
			while (seen < value && !queue.completed.compare_exchange_weak(seen, value,
			                           std::memory_order_acq_rel, std::memory_order_relaxed))
			{
			}
		}

		void poll(Backend& b)
		{
			for (u32 i = 0; i < b.ctx.queue_count; ++i)
			{
				u64 value = 0;
				if (vkGetSemaphoreCounterValue(b.ctx.device, b.ctx.queues[i].timeline, &value) ==
				    VK_SUCCESS)
				{
					raise_completed(b.ctx.queues[i], value);
				}
			}

			for (u32 i = 0; i < b.ctx.in_flight; ++i)
			{
				Slot& slot = b.slots[i];
				if (slot.frame != nullptr && slot.closed &&
				    !slot.complete.load(std::memory_order_acquire) && values_passed(b, slot))
				{
					complete_slot(b, slot);
				}
			}
		}

		void run_blocking(jobs::JobFn fn, void* data, const char* name)
		{
			// A job or main parks while an IO thread blocks. A thread outside the scheduler blocks
			// here itself, as does everyone when the IO lane is full or not running.
			if (jobs::worker_index() != jobs::NO_WORKER)
			{
				jobs::Counter done;
				const jobs::JobDef job{.fn = fn,
				    .data = data,
				    .name = name,
				    .priority = jobs::JobPriority::High};

				if (jobs::kick_io(job, done) == jobs::IoKickError::None)
				{
					jobs::wait(done);
					return;
				}
			}

			fn(data);
		}

		void wait_value(Backend& b, QueueState& queue, u64 value)
		{
			if (value == 0 || queue.completed.load(std::memory_order_acquire) >= value ||
			    b.lost.load(std::memory_order_acquire))
			{
				return;
			}

			// A poll first: the common case is a frame that finished while the CPU ran ahead.
			u64 seen = 0;
			if (vkGetSemaphoreCounterValue(b.ctx.device, queue.timeline, &seen) == VK_SUCCESS)
			{
				raise_completed(queue, seen);
				if (seen >= value)
				{
					return;
				}
			}

			EMBER_PROFILE_SCOPE_C("gpu: wait", PROFILE_COLOR_WAIT);

			TimelineWait wait{.device = b.ctx.device, .semaphore = queue.timeline, .value = value};
			run_blocking(timeline_wait_job, &wait, "gpu wait");
			note_result(b, wait.result);

			if (wait.result == VK_SUCCESS)
			{
				raise_completed(queue, value);
			}
		}

		void bury(Backend& b, Dead dead)
		{
			dead.frame = b.latest_frame;
			b.graveyard.push_back(dead);
		}

		void drain(Backend& b, bool everything)
		{
			b.resources_lock.lock();

			// A frame begun and not complete keeps everything stamped with it or later.
			u64 threshold = UINT64_MAX;
			if (!everything)
			{
				for (u32 i = 0; i < b.ctx.in_flight; ++i)
				{
					const Slot& slot = b.slots[i];
					if (slot.frame != nullptr && !slot.complete.load(std::memory_order_acquire) &&
					    slot.frame_index < threshold)
					{
						threshold = slot.frame_index;
					}
				}
			}

			while (b.graveyard_head < b.graveyard.size() &&
			       b.graveyard[b.graveyard_head].frame < threshold)
			{
				release(b, b.graveyard[b.graveyard_head]);
				++b.graveyard_head;
			}

			// Compact when empty, or once the dead prefix is long; survivors span only the frames
			// in flight, so this stays rare.
			if (b.graveyard_head == b.graveyard.size())
			{
				b.graveyard.clear();
				b.graveyard_head = 0;
			}
			else if (b.graveyard_head >= 256)
			{
				b.graveyard.erase(b.graveyard.begin(), b.graveyard.begin() + b.graveyard_head);
				b.graveyard_head = 0;
			}

			b.resources_lock.unlock();
		}
	}

	const Caps& caps()
	{
		static const Caps NONE = {};
		const vk::Backend* b = vk::backend();
		return b != nullptr ? b->ctx.caps : NONE;
	}

	bool is_lost()
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->lost.load(std::memory_order_acquire);
	}

	void begin_frame(Frame& frame)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		EMBER_PROFILE_SCOPE_C("gpu: begin_frame", PROFILE_COLOR_RENDER);

		vk::Slot& slot = b->slots[frame.index % b->ctx.in_flight];

		// A loop that retires without waiting pays here instead.
		if (slot.frame != nullptr && !slot.complete.load(std::memory_order_acquire))
		{
			wait_slot(*b, slot);
		}

		// Under the lock a destroy stamps with, so a destroy lands wholly before or after.
		b->resources_lock.lock();
		b->latest_frame = frame.index;
		b->resources_lock.unlock();

		const u32 opened = slot.opened.load(std::memory_order_acquire);
		const u32 count = opened < b->ctx.max_lists ? opened : b->ctx.max_lists;

		for (u32 i = 0; i < count; ++i)
		{
			EMBER_VK_CHECK(vkResetCommandPool(b->ctx.device, slot.lists[i].pool, 0));
			reset_list(slot.lists[i]);
		}

		EMBER_VK_CHECK(vkResetCommandPool(b->ctx.device, slot.finish.pool, 0));
		reset_list(slot.finish);

		if (b->timestamps != VK_NULL_HANDLE)
		{
			const u32 per_slot = (b->ctx.max_lists + 1) * vk::MAX_ZONES * 2;
			vkResetQueryPool(b->ctx.device, b->timestamps, slot_index(*b, slot) * per_slot,
			    per_slot);
		}

		slot.opened.store(0, std::memory_order_release);
		slot.frame = &frame;
		slot.frame_index = frame.index;
		slot.closed = false;
		slot.present_count = 0;
		slot.acquire_wait_count = 0;
		slot.zone_count = 0;
		for (u64& value : slot.values)
		{
			value = 0;
		}
		slot.complete.store(false, std::memory_order_release);

		vk::poll(*b);
		vk::drain(*b, false);
	}

	void end_frame(Frame& frame)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		EMBER_PROFILE_SCOPE_C("gpu: end_frame", PROFILE_COLOR_RENDER);

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(slot.frame == &frame && !slot.closed, "end_frame of a frame not open");

		const u32 opened = slot.opened.load(std::memory_order_acquire);
		const u32 count = opened < b->ctx.max_lists ? opened : b->ctx.max_lists;

		for (u32 i = 0; i < count; ++i)
		{
			const CommandList& list = slot.lists[i];
			EMBER_ASSERT_MSG(!list.open, "a command list was still recording at end_frame");
			if (list.ended && !list.submitted)
			{
				EMBER_ERROR("gpu: frame {} ended command list '{}' but never submitted it",
				    frame.index, list.name != nullptr ? list.name : "?");
			}
		}

		// The frame's own list: whatever it acquired is moved to Present.
		CommandList& finish = slot.finish;
		finish.frame_index = frame.index;
		finish.open = true;

		const VkCommandBufferBeginInfo begin{
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(finish.commands, &begin));
		vk::record_finish(*b, slot, finish.commands);
		EMBER_VK_CHECK(vkEndCommandBuffer(finish.commands));
		finish.open = false;
		finish.ended = true;
		finish.submitted = true;

		// The last graphics submit: it waits on any acquire no earlier submit consumed, signals
		// each acquired image's present semaphore and the timeline value the frame completes at.
		vk::QueueState& queue = vk::queue_for(*b, Queue::Graphics);
		const u32 queue_index = b->ctx.queue_index[(u32)Queue::Graphics];

		VkSemaphoreSubmitInfo waits[vk::MAX_SWAPCHAINS];
		u32 wait_count = 0;

		slot.lock.lock();
		for (u32 i = 0; i < slot.acquire_wait_count; ++i)
		{
			waits[wait_count++] = {
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			    .semaphore = slot.acquire_waits[i],
			    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			};
		}
		slot.acquire_wait_count = 0;
		slot.lock.unlock();

		VkSemaphoreSubmitInfo signals[vk::MAX_SWAPCHAINS + 1];
		u32 signal_count = vk::present_signals(*b, slot, signals);

		const VkCommandBufferSubmitInfo commands{
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		    .commandBuffer = finish.commands,
		};

		queue.lock.lock();
		const u64 value = ++queue.submitted;
		signals[signal_count++] = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		    .semaphore = queue.timeline,
		    .value = value,
		    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		const VkSubmitInfo2 submit_info{
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		    .waitSemaphoreInfoCount = wait_count,
		    .pWaitSemaphoreInfos = waits,
		    .commandBufferInfoCount = 1,
		    .pCommandBufferInfos = &commands,
		    .signalSemaphoreInfoCount = signal_count,
		    .pSignalSemaphoreInfos = signals,
		};

		vk::note_result(*b, vkQueueSubmit2(queue.handle, 1, &submit_info, VK_NULL_HANDLE));
		slot.values[queue_index] = value;
		queue.lock.unlock();

		slot.closed = true;
		frames::begin_stage(frame, Stage::Gpu);

		vk::present(*b, slot);
	}

	void wait_frame(Frame& frame)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(slot.frame == &frame && slot.closed, "wait_frame before end_frame");
		wait_slot(*b, slot);
	}

	CommandList* begin_command_list(const Frame& frame, const char* name)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return nullptr;
		}

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(!slot.closed, "a command list for a frame that has ended");

		const u32 index = slot.opened.fetch_add(1, std::memory_order_acq_rel);
		if (index >= b->ctx.max_lists)
		{
			if (index == b->ctx.max_lists)
			{
				EMBER_ERROR("gpu: frame {} opened more than {} command lists", frame.index,
				    b->ctx.max_lists);
			}
			return nullptr;
		}

		CommandList& list = slot.lists[index];
		list.frame_index = frame.index;
		list.name = name;
		list.open = true;

		const VkCommandBufferBeginInfo begin{
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(list.commands, &begin));

		if (b->ctx.debug_utils && name != nullptr)
		{
			const VkDebugUtilsLabelEXT label{
			    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			    .pLabelName = name,
			    .color = {1.0f, 1.0f, 1.0f, 1.0f},
			};
			vkCmdBeginDebugUtilsLabelEXT(list.commands, &label);
		}

		return &list;
	}

	void end_command_list(CommandList* list)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr || list == nullptr)
		{
			return;
		}

		EMBER_ASSERT_MSG(list->open, "end of a list that is not recording");
		EMBER_ASSERT_MSG(!list->inside_pass, "a render pass is still open");
		EMBER_ASSERT_MSG(list->zone_depth == 0, "a GPU zone was begun but never ended");

		if (b->ctx.debug_utils && list->name != nullptr)
		{
			vkCmdEndDebugUtilsLabelEXT(list->commands);
		}

		EMBER_VK_CHECK(vkEndCommandBuffer(list->commands));
		list->open = false;
		list->ended = true;
	}

	Submission submit(const Frame& frame, const SubmitDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		EMBER_PROFILE_SCOPE_C("gpu: submit", PROFILE_COLOR_RENDER);
		EMBER_ASSERT(def.queue < Queue::Count);
		EMBER_ASSERT(def.waits.size() + vk::MAX_SWAPCHAINS <= MAX_WAITS);

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(!slot.closed, "a submit for a frame that has ended");

		for (CommandList* list : def.lists)
		{
			EMBER_ASSERT_MSG(list != nullptr && list->frame_index == frame.index,
			    "a list of another frame");
			EMBER_ASSERT_MSG(list->ended && !list->submitted,
			    "a list not ended, or submitted twice");
			list->submitted = true;
		}

		VkSemaphoreSubmitInfo waits[MAX_WAITS];
		u32 wait_count = 0;

		for (const Submission& wait : def.waits)
		{
			if (wait.value == 0)
			{
				continue;
			}

			waits[wait_count++] = {
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			    .semaphore = vk::queue_for(*b, wait.queue).timeline,
			    .value = wait.value,
			    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			};
		}

		// The first graphics submit after an acquire waits for the display to hand the image
		// over. Later submits on the queue run behind it, so one wait covers them all.
		if (def.queue == Queue::Graphics)
		{
			slot.lock.lock();
			for (u32 i = 0; i < slot.acquire_wait_count; ++i)
			{
				waits[wait_count++] = {
				    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				    .semaphore = slot.acquire_waits[i],
				    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
			}
			slot.acquire_wait_count = 0;
			slot.lock.unlock();
		}

		vk::QueueState& queue = vk::queue_for(*b, def.queue);
		const u32 queue_index = b->ctx.queue_index[(u32)def.queue];

		queue.lock.lock();
		const u64 value = ++queue.submitted;

		const VkSemaphoreSubmitInfo signal{
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		    .semaphore = queue.timeline,
		    .value = value,
		    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		const u32 total = (u32)def.lists.size();
		u32 offset = 0;
		do
		{
			const u32 remaining = total - offset;
			const u32 count = remaining < LISTS_PER_SUBMIT ? remaining : LISTS_PER_SUBMIT;
			const bool first = offset == 0;
			const bool last = offset + count == total;

			VkCommandBufferSubmitInfo commands[LISTS_PER_SUBMIT];
			for (u32 i = 0; i < count; ++i)
			{
				commands[i] = {
				    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				    .commandBuffer = def.lists[offset + i]->commands,
				};
			}

			const VkSubmitInfo2 submit_info{
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			    .waitSemaphoreInfoCount = first ? wait_count : 0,
			    .pWaitSemaphoreInfos = waits,
			    .commandBufferInfoCount = count,
			    .pCommandBufferInfos = commands,
			    .signalSemaphoreInfoCount = last ? 1u : 0u,
			    .pSignalSemaphoreInfos = &signal,
			};

			vk::note_result(*b, vkQueueSubmit2(queue.handle, 1, &submit_info, VK_NULL_HANDLE));
			offset += count;
		} while (offset < total);

		slot.values[queue_index] = value;
		queue.lock.unlock();

		return {.value = value, .queue = def.queue};
	}

	bool is_complete(Submission submission)
	{
		vk::Backend* b = vk::backend();
		if (submission.value == 0 || b == nullptr)
		{
			return true;
		}

		vk::QueueState& queue = vk::queue_for(*b, submission.queue);
		if (queue.completed.load(std::memory_order_acquire) >= submission.value)
		{
			return true;
		}

		u64 seen = 0;
		if (vkGetSemaphoreCounterValue(b->ctx.device, queue.timeline, &seen) == VK_SUCCESS)
		{
			vk::raise_completed(queue, seen);
		}

		return seen >= submission.value;
	}

	void wait(Submission submission)
	{
		vk::Backend* b = vk::backend();
		if (submission.value == 0 || b == nullptr)
		{
			return;
		}

		vk::wait_value(*b, vk::queue_for(*b, submission.queue), submission.value);
		vk::poll(*b);
	}

	void wait_idle()
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		EMBER_PROFILE_SCOPE_C("gpu: wait_idle", PROFILE_COLOR_WAIT);

		IdleWait wait{.device = b->ctx.device};
		vk::run_blocking(idle_job, &wait, "gpu idle");
		vk::note_result(*b, wait.result);

		// Idle proves every value handed out has signalled.
		for (u32 i = 0; i < b->ctx.queue_count; ++i)
		{
			vk::QueueState& queue = b->ctx.queues[i];
			queue.lock.lock();
			const u64 submitted = queue.submitted;
			queue.lock.unlock();
			vk::raise_completed(queue, submitted);
		}

		vk::poll(*b);
		vk::drain(*b, true);
	}

	Span<const ZoneTiming> zones(const Frame& frame)
	{
		const vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		const vk::Slot& slot = b->slots[frame.index % b->ctx.in_flight];
		if (slot.frame_index != frame.index || !slot.complete.load(std::memory_order_acquire))
		{
			return {};
		}

		return {slot.zones, slot.zone_count};
	}

	GpuStats stats()
	{
		vk::Backend* b = vk::backend();
		GpuStats stats;
		if (b == nullptr)
		{
			return stats;
		}

		b->resources_lock.lock();
		stats.textures = b->textures.size();
		stats.pending_destroys = (u32)(b->graveyard.size() - b->graveyard_head);
		b->resources_lock.unlock();

		VmaTotalStatistics memory_stats = {};
		vmaCalculateStatistics(b->ctx.allocator, &memory_stats);
		stats.device_memory_bytes = memory_stats.total.statistics.allocationBytes;

		for (u32 i = 0; i < b->ctx.in_flight; ++i)
		{
			const u32 opened = b->slots[i].opened.load(std::memory_order_relaxed);
			stats.command_lists += opened < b->ctx.max_lists ? opened : b->ctx.max_lists;
		}

		return stats;
	}
}
