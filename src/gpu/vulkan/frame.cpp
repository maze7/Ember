// Gives each frame a reusable slot for its command lists, uploads, and pending presents.
// begin_frame() waits before reusing that storage; end_frame() closes it to further recording
// and submission. The slot records the last timeline value submitted to each queue, and the
// frame loop's fiber alone turns a closed slot into a complete one, so completion never races
// a slot's reuse. Jobs record, upload, submit and wait on queue points.
//
// Resource destruction uses those frame completions too. Handles retire immediately, while
// native objects and descriptor slots wait in the graveyard until their frame stamp can pass.

#include <ember/gpu/gpu.h>

#include <ember/core/frame.h>
#include <ember/core/jobs.h>
#include <ember/core/profile.h>
#include <ember/math/scalar.h>
#include <ember/memory/heap.h>
#include <gpu/vulkan/backend.h>

#if defined(EMBER_HAS_PLATFORM)
	#include <platform/wsi.h>
#endif

namespace ember::gpu
{
	namespace
	{
		// Bound the temporary command-buffer array on the stack. Larger submits keep the queue
		// lock while sending consecutive chunks.
		constexpr u32 LISTS_PER_SUBMIT = 32;
		constexpr u32 MAX_WAITS = 16;

		// Move a small batch out of the graveyard, then drop the lock before calling the driver.
		// More retired objects take another pass without growing the stack allocation.
		constexpr u32 DRAIN_BATCH = 32;

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

		// Clear recording state for the next frame while keeping the preallocated native objects
		// and the list's place in the slot.
		void reset_list(CommandList& list)
		{
			list = CommandList{.pool = list.pool,
			    .commands = list.commands,
			    .queue = list.queue,
			    .slot = list.slot,
			    .index = list.index};
		}

		u32 slot_index(const vk::Backend& b, const vk::Slot& slot)
		{
			return (u32)(&slot - b.slots);
		}

		// Lists the slot has opened in one queue's table, capped at the table's capacity because
		// the counter keeps climbing past it.
		u32 opened_count(const vk::Backend& b, const vk::Slot& slot, u32 queue)
		{
			return min(slot.opened[queue].load(std::memory_order_acquire),
			    b.ctx.list_capacity[queue]);
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

		// Collect timestamp pairs for one list after the frame's GPU work has finished. The query
		// results provide durations; names and nesting stayed with the command list on the CPU.
		void resolve_list_zones(vk::Backend& b, vk::Slot& slot, const CommandList& list)
		{
			if (list.zone_count == 0)
			{
				return;
			}

			const u32 base =
			    ((slot_index(b, slot) * (b.ctx.list_total + 1)) + list.index) * vk::MAX_ZONES * 2;
			u64 ticks[vk::MAX_ZONES * 2];

			if (vkGetQueryPoolResults(b.ctx.device, b.timestamps, base, list.zone_count * 2,
			        sizeof(u64) * list.zone_count * 2, ticks, sizeof(u64),
			        VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
			{
				return;
			}

			const f32 period = b.ctx.caps.timestamp_period_ns;
			for (u32 zone = 0; zone < list.zone_count; ++zone)
			{
				slot.zones[slot.zone_count++] = {
				    .name = list.zones[zone].name,
				    .color = list.zones[zone].color,
				    .depth = list.zones[zone].depth,
				    .duration_ms = (f32)(ticks[zone * 2 + 1] - ticks[zone * 2]) * period / 1.0e6f,
				};
			}
		}

		void resolve_zones(vk::Backend& b, vk::Slot& slot)
		{
			slot.zone_count = 0;
			if (b.timestamps == VK_NULL_HANDLE)
			{
				return;
			}

			for (u32 q = 0; q < vk::QUEUE_COUNT; ++q)
			{
				const u32 count = opened_count(b, slot, q);
				for (u32 i = 0; i < count; ++i)
				{
					resolve_list_zones(b, slot, slot.lists[q][i]);
				}
			}

			resolve_list_zones(b, slot, slot.finish);
		}

		// Turn a Closed slot whose queue values have passed into Complete: resolve its zones and
		// end its Stage::Gpu. Only the frame loop's fiber calls this, so nothing can claim or
		// reuse the slot meanwhile; other threads only read the state.
		void finish_slot(vk::Backend& b, vk::Slot& slot)
		{
			EMBER_ASSERT(jobs::is_main());
			EMBER_ASSERT(slot.state.load(std::memory_order_relaxed) == vk::SlotState::Closed);

			resolve_zones(b, slot);
			frames::end_stage(*slot.frame, Stage::Gpu);
			slot.state.store(vk::SlotState::Complete, std::memory_order_release);
		}

		// Wait for a Closed slot's GPU work and complete it.
		void wait_slot(vk::Backend& b, vk::Slot& slot)
		{
			for (u32 i = 0; i < b.ctx.queue_count; ++i)
			{
				vk::wait_value(b, b.ctx.queues[i], slot.values[i]);
			}

			finish_slot(b, slot);
		}

		// Move the slot's pending acquire semaphores into a submission's wait list. The caller
		// holds the queue lock, so the submission taking them is the first on the queue after
		// acquisition, and everything later in submission order is ordered behind them too.
		u32 take_acquire_waits(vk::Slot& slot, VkSemaphoreSubmitInfo* waits)
		{
			slot.lock.lock();
			const u32 count = slot.acquire_wait_count;
			for (u32 i = 0; i < count; ++i)
			{
				waits[i] = {
				    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
				    .semaphore = slot.acquire_waits[i],
				    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				};
			}
			slot.acquire_wait_count = 0;
			slot.lock.unlock();
			return count;
		}

		// Apply the backbuffer states the lists recorded to the slot's pending presents, in the
		// order the lists are being submitted. The caller holds the queue lock, so that order is
		// the one the GPU will see. An entry with no pending present names an image this frame
		// did not acquire and is ignored.
		void fold_backbuffer_states(vk::Slot& slot, Span<CommandList* const> lists)
		{
			slot.lock.lock();
			for (const CommandList* list : lists)
			{
				for (u32 i = 0; i < list->backbuffer_count; ++i)
				{
					const CommandList::BackbufferUse& use = list->backbuffers[i];
					for (u32 p = 0; p < slot.present_count; ++p)
					{
						vk::PendingPresent& pending = slot.presents[p];
						if (pending.swapchain == use.swapchain && pending.image == use.image)
						{
							pending.state = use.state;
						}
					}
				}
			}
			slot.lock.unlock();
		}

		// Descriptor slots must be reset before their indices return to the pool. Do both under
		// resources_lock so creation cannot reuse an index halfway through its reset.
		bool is_slot(vk::Dead::Kind kind)
		{
			return kind == vk::Dead::Kind::TextureSlot || kind == vk::Dead::Kind::BufferSlot ||
			       kind == vk::Dead::Kind::SamplerSlot;
		}

		void release_slot(vk::Backend& b, const vk::Dead& dead)
		{
			switch (dead.kind)
			{
				case vk::Dead::Kind::TextureSlot:
					vk::heap_reset_texture(b, (u32)dead.handle, b.texture_cold[dead.handle].type);
					b.textures.release((u32)dead.handle);
					break;
				case vk::Dead::Kind::BufferSlot:
					vk::heap_reset_buffer(b, (u32)dead.handle);
					b.buffers.release((u32)dead.handle);
					break;
				case vk::Dead::Kind::SamplerSlot:
					vk::heap_reset_sampler(b, (u32)dead.handle);
					b.samplers.release((u32)dead.handle);
					break;
				default:
					EMBER_ASSERT_MSG(false, "not a slot");
					break;
			}
		}

		// Driver destruction can take time, so release native objects after dropping the graveyard
		// lock. CPU allocations retired with the resource follow the same path.
		void release_native(vk::Backend& b, const vk::Dead& dead)
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
				case vk::Dead::Kind::Fence:
					vkDestroyFence(b.ctx.device, (VkFence)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Swapchain:
					vkDestroySwapchainKHR(b.ctx.device, (VkSwapchainKHR)dead.handle, callbacks);
					break;
				case vk::Dead::Kind::Surface:
					// Match the platform's surface creation path, including its allocation
					// callbacks.
#if defined(EMBER_HAS_PLATFORM)
					platform::vulkan::destroy_surface(b.ctx.instance, (VkSurfaceKHR)dead.handle);
#else
					vkDestroySurfaceKHR(b.ctx.instance, (VkSurfaceKHR)dead.handle, nullptr);
#endif
					break;
				case vk::Dead::Kind::Memory:
					memory::free((void*)dead.handle);
					break;
				case vk::Dead::Kind::TextureSlot:
				case vk::Dead::Kind::BufferSlot:
				case vk::Dead::Kind::SamplerSlot:
					EMBER_ASSERT_MSG(false, "a slot is released under the lock");
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

		void poll_queues(Backend& b)
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
		}

		void finish_frames(Backend& b)
		{
			EMBER_ASSERT(jobs::is_main());

			for (u32 i = 0; i < b.ctx.in_flight; ++i)
			{
				Slot& slot = b.slots[i];
				if (slot.state.load(std::memory_order_acquire) == SlotState::Closed &&
				    values_passed(b, slot))
				{
					finish_slot(b, slot);
				}
			}
		}

		bool any_frame_open(const Backend& b)
		{
			for (u32 i = 0; i < b.ctx.in_flight; ++i)
			{
				if (is_open(b.slots[i]))
				{
					return true;
				}
			}

			return false;
		}

		void wait_frames(Backend& b)
		{
			EMBER_ASSERT(jobs::is_main());
			EMBER_ASSERT_MSG(!any_frame_open(b), "waiting out the frames while one is open");

			for (u32 i = 0; i < b.ctx.in_flight; ++i)
			{
				Slot& slot = b.slots[i];
				if (slot.state.load(std::memory_order_acquire) == SlotState::Closed)
				{
					wait_slot(b, slot);
				}
			}
		}

		void run_blocking(jobs::JobFn fn, void* data, const char* name)
		{
			// Let an I/O thread do the blocking call while a scheduler fiber waits on its counter.
			// Without an available I/O lane, or outside the scheduler, the caller does the work
			// itself.
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

			// Check the timeline before scheduling a blocking wait. The GPU may already have caught
			// up while the CPU was doing other work.
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
			Dead batch[DRAIN_BATCH];
			u32 count = 0;

			do
			{
				count = 0;
				b.resources_lock.lock();

				// The oldest unfinished frame holds back its stamp and every later one. Retired
				// objects with an earlier stamp can leave the graveyard.
				u64 threshold = UINT64_MAX;
				if (!everything)
				{
					for (u32 i = 0; i < b.ctx.in_flight; ++i)
					{
						const Slot& slot = b.slots[i];
						const SlotState state = slot.state.load(std::memory_order_acquire);
						if (state != SlotState::Idle && state != SlotState::Complete &&
						    slot.frame_index < threshold)
						{
							threshold = slot.frame_index;
						}
					}
				}

				// Return descriptor slots under the lock. Keep native objects in the stack batch
				// until the lock is released; a full batch leaves the rest for the next pass.
				while (b.graveyard_head < b.graveyard.size() &&
				       b.graveyard[b.graveyard_head].frame < threshold)
				{
					const Dead& dead = b.graveyard[b.graveyard_head];
					if (is_slot(dead.kind))
					{
						release_slot(b, dead);
					}
					else if (count < DRAIN_BATCH)
					{
						batch[count++] = dead;
					}
					else
					{
						break;
					}
					++b.graveyard_head;
				}

				// Drop the consumed prefix occasionally instead of shifting survivors after every
				// drain. An empty graveyard can keep its allocation for the next frame.
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

				for (u32 i = 0; i < count; ++i)
				{
					release_native(b, batch[i]);
				}
			} while (count == DRAIN_BATCH);
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
		EMBER_ASSERT_MSG(jobs::is_main(), "frames are driven from the frame loop's fiber");

		vk::Slot& slot = b->slots[frame.index % b->ctx.in_flight];

		// Reusing a slot is the last point where its previous frame can finish. Wait before
		// resetting command pools, query slots, and mapped ring storage.
		const vk::SlotState previous = slot.state.load(std::memory_order_acquire);
		EMBER_ASSERT_MSG(previous != vk::SlotState::Open, "begin_frame over a frame still open");
		if (previous == vk::SlotState::Closed)
		{
			wait_slot(*b, slot);
		}

		// Destruction takes this lock when stamping a resource, so each retirement sees either
		// the previous frame or this one.
		b->resources_lock.lock();
		b->latest_frame = frame.index;
		b->resources_lock.unlock();

		for (u32 q = 0; q < vk::QUEUE_COUNT; ++q)
		{
			const u32 count = opened_count(*b, slot, q);
			for (u32 i = 0; i < count; ++i)
			{
				CommandList& list = slot.lists[q][i];
				EMBER_VK_CHECK(vkResetCommandPool(b->ctx.device, list.pool, 0));
				reset_list(list);
			}
			slot.opened[q].store(0, std::memory_order_release);
		}

		EMBER_VK_CHECK(vkResetCommandPool(b->ctx.device, slot.finish.pool, 0));
		reset_list(slot.finish);

		EMBER_VK_CHECK(vkResetCommandPool(b->ctx.device, slot.upload_pool, 0));
		slot.upload_batch = 0;
		slot.upload_open = false;
		slot.upload_value = 0;
		vk::ring_reset(b->transient, slot_index(*b, slot));
		vk::ring_reset(b->staging, slot_index(*b, slot));

		if (b->timestamps != VK_NULL_HANDLE)
		{
			const u32 per_slot = (b->ctx.list_total + 1) * vk::MAX_ZONES * 2;
			vkResetQueryPool(b->ctx.device, b->timestamps, slot_index(*b, slot) * per_slot,
			    per_slot);
		}

		slot.frame = &frame;
		slot.frame_index = frame.index;
		slot.present_count = 0;
		slot.acquire_wait_count = 0;
		slot.zone_count = 0;
		for (u64& value : slot.values)
		{
			value = 0;
		}
		slot.state.store(vk::SlotState::Open, std::memory_order_release);

		vk::poll_queues(*b);
		vk::finish_frames(*b);
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
		EMBER_ASSERT_MSG(jobs::is_main(), "frames are driven from the frame loop's fiber");

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(slot.frame == &frame && vk::is_open(slot),
		    "end_frame of a frame not open");

		for (u32 q = 0; q < vk::QUEUE_COUNT; ++q)
		{
			const u32 count = opened_count(*b, slot, q);
			for (u32 i = 0; i < count; ++i)
			{
				const CommandList& list = slot.lists[q][i];
				EMBER_ASSERT_MSG(!list.open, "a command list was still recording at end_frame");
				if (list.ended && !list.submitted)
				{
					EMBER_ERROR("gpu: frame {} ended command list '{}' but never submitted it",
					    frame.index, list.name != nullptr ? list.name : "?");
				}
			}
		}

		// Use the slot's private list for the acquired images' final transitions. It also clears
		// an image that the frame left unused.
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

		// Finish graphics with any acquire waits still pending, each image's present signal, and
		// a timeline signal covering this submission. Other queues keep their own frame values.
		vk::QueueState& queue = vk::queue_for(*b, Queue::Graphics);
		const u32 queue_index = b->ctx.queue_index[(u32)Queue::Graphics];

		VkSemaphoreSubmitInfo signals[vk::MAX_SWAPCHAINS + 1];
		u32 signal_count = vk::present_signals(*b, slot, signals);

		b->last_transient_used = vk::ring_used(b->transient, slot_index(*b, slot));
		b->last_staging_used = vk::ring_used(b->staging, slot_index(*b, slot));
		b->peak_transient_used = max(b->peak_transient_used, b->last_transient_used);
		b->peak_staging_used = max(b->peak_staging_used, b->last_staging_used);

		// The GPU stage starts with the frame's last submission. Stamp it before the slot closes.
		frames::begin_stage(frame, Stage::Gpu);

		queue.lock.lock();

		// Uploads nobody submitted ride ahead of the finish list, taken under the queue lock like
		// submit_uploads() takes them, so nothing can reach the queue between the two.
		VkCommandBufferSubmitInfo commands[2];
		u32 command_count = 0;
		if (const VkCommandBuffer uploads = vk::flush_uploads(slot); uploads != VK_NULL_HANDLE)
		{
			commands[command_count++] = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			    .commandBuffer = uploads,
			};
		}
		commands[command_count++] = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		    .commandBuffer = finish.commands,
		};

		VkSemaphoreSubmitInfo waits[vk::MAX_SWAPCHAINS];
		const u32 wait_count = take_acquire_waits(slot, waits);

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
		    .commandBufferInfoCount = command_count,
		    .pCommandBufferInfos = commands,
		    .signalSemaphoreInfoCount = signal_count,
		    .pSignalSemaphoreInfos = signals,
		};

		vk::note_result(*b, vkQueueSubmit2(queue.handle, 1, &submit_info, VK_NULL_HANDLE));
		slot.values[queue_index] = value;
		slot.upload_value = value;
		queue.lock.unlock();

		slot.state.store(vk::SlotState::Closed, std::memory_order_release);

		// Measure the CPU call that queues presentation. Its semaphore waits happen on the GPU,
		// so this stage records the handoff rather than display latency.
		frames::begin_stage(frame, Stage::Present);
		vk::present(*b, slot);
		frames::end_stage(frame, Stage::Present);
	}

	void wait_frame(Frame& frame)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		EMBER_ASSERT_MSG(jobs::is_main(), "frames are driven from the frame loop's fiber");

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(slot.frame == &frame && !vk::is_open(slot), "wait_frame before end_frame");
		if (slot.state.load(std::memory_order_acquire) == vk::SlotState::Closed)
		{
			wait_slot(*b, slot);
		}
	}

	CommandList* begin_command_list(const Frame& frame, const char* name, Queue queue)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return nullptr;
		}

		EMBER_ASSERT(queue < Queue::Count);

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(vk::is_open(slot), "a command list for a frame that has ended");

		const u32 q = (u32)queue;
		const u32 capacity = b->ctx.list_capacity[q];
		const u32 index = slot.opened[q].fetch_add(1, std::memory_order_acq_rel);
		if (index >= capacity)
		{
			if (index == capacity)
			{
				constexpr const char* NAMES[vk::QUEUE_COUNT] = {"graphics", "compute", "transfer"};
				EMBER_ERROR("gpu: frame {} opened more than {} {} command lists", frame.index,
				    capacity, NAMES[q]);
			}
			return nullptr;
		}

		CommandList& list = slot.lists[q][index];
		list.frame_index = frame.index;
		list.name = name;
		list.open = true;

		const VkCommandBufferBeginInfo begin{
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		EMBER_VK_CHECK(vkBeginCommandBuffer(list.commands, &begin));

		// Start each bind point the queue supports with the shared heap and offsets into the
		// zeroed constant block. Later set_constants() calls replace those offsets lazily. A
		// transfer queue has no bind points.
		if (queue != Queue::Transfer)
		{
			const VkDescriptorSet sets[] = {b->heap.set, b->heap.constants};
			const u32 zero_offsets[CONSTANT_SLOTS] = {};
			if (queue == Queue::Graphics)
			{
				vkCmdBindDescriptorSets(list.commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
				    b->heap.pipeline_layout, 0, 2, sets, CONSTANT_SLOTS, zero_offsets);
			}
			vkCmdBindDescriptorSets(list.commands, VK_PIPELINE_BIND_POINT_COMPUTE,
			    b->heap.pipeline_layout, 0, 2, sets, CONSTANT_SLOTS, zero_offsets);
		}

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
		EMBER_ASSERT_MSG(vk::is_open(slot), "a submit for a frame that has ended");

		for (CommandList* list : def.lists)
		{
			EMBER_ASSERT_MSG(list != nullptr && list->frame_index == frame.index,
			    "a list of another frame");
			EMBER_ASSERT_MSG(list->ended && !list->submitted,
			    "a list not ended, or submitted twice");
			EMBER_ASSERT_MSG(list->queue == def.queue, "a list begun for another queue");
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

		vk::QueueState& queue = vk::queue_for(*b, def.queue);
		const u32 queue_index = b->ctx.queue_index[(u32)def.queue];

		queue.lock.lock();

		// A graphics submission consumes the pending acquire semaphores, so its image accesses
		// wait for acquisition, and records the states its lists leave the acquired images in.
		// Both happen under the queue lock because both must follow submission order rather than
		// the order in which callers reached this function.
		if (def.queue == Queue::Graphics)
		{
			wait_count += take_acquire_waits(slot, waits + wait_count);
			fold_backbuffer_states(slot, def.lists);
		}

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
			const u32 count = min(remaining, LISTS_PER_SUBMIT);
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

	Submission submit_uploads(const Frame& frame)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		EMBER_PROFILE_SCOPE_C("gpu: submit_uploads", PROFILE_COLOR_RENDER);

		vk::Slot& slot = vk::slot_of(*b, frame.index);
		EMBER_ASSERT_MSG(vk::is_open(slot), "an upload submit for a frame that has ended");

		vk::QueueState& queue = vk::queue_for(*b, Queue::Graphics);
		const u32 queue_index = b->ctx.queue_index[(u32)Queue::Graphics];

		// Take the batch and submit it under the queue lock, so the point returned covers every
		// upload recorded before this call, including one that another thread's call swept into
		// the batch it submitted first.
		queue.lock.lock();
		if (const VkCommandBuffer uploads = vk::flush_uploads(slot); uploads != VK_NULL_HANDLE)
		{
			const u64 value = ++queue.submitted;
			const VkSemaphoreSubmitInfo signal{
			    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
			    .semaphore = queue.timeline,
			    .value = value,
			    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			};
			const VkCommandBufferSubmitInfo commands{
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
			    .commandBuffer = uploads,
			};
			const VkSubmitInfo2 submit_info{
			    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			    .commandBufferInfoCount = 1,
			    .pCommandBufferInfos = &commands,
			    .signalSemaphoreInfoCount = 1,
			    .pSignalSemaphoreInfos = &signal,
			};

			vk::note_result(*b, vkQueueSubmit2(queue.handle, 1, &submit_info, VK_NULL_HANDLE));
			slot.values[queue_index] = value;
			slot.upload_value = value;
		}
		const u64 covered = slot.upload_value;
		queue.lock.unlock();

		return {.value = covered, .queue = Queue::Graphics};
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
	}

	void wait_idle()
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return;
		}

		EMBER_PROFILE_SCOPE_C("gpu: wait_idle", PROFILE_COLOR_WAIT);
		EMBER_ASSERT_MSG(jobs::is_main(), "frames are driven from the frame loop's fiber");

		IdleWait wait{.device = b->ctx.device};
		vk::run_blocking(idle_job, &wait, "gpu idle");
		vk::note_result(*b, wait.result);

		// A successful idle wait covers every submitted value. Refresh the cached counters before
		// polling frame completion and draining the graveyard.
		for (u32 i = 0; i < b->ctx.queue_count; ++i)
		{
			vk::QueueState& queue = b->ctx.queues[i];
			queue.lock.lock();
			const u64 submitted = queue.submitted;
			queue.lock.unlock();
			vk::raise_completed(queue, submitted);
		}

		vk::finish_frames(*b);
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
		if (slot.frame_index != frame.index ||
		    slot.state.load(std::memory_order_acquire) != vk::SlotState::Complete)
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
		stats.buffers = b->buffers.size();
		stats.samplers = b->samplers.size();
		stats.pipelines = b->graphics_pipelines.size() + b->compute_pipelines.size();
		stats.pending_destroys = (u32)(b->graveyard.size() - b->graveyard_head);
		b->resources_lock.unlock();

		stats.transient_bytes = b->last_transient_used;
		stats.staging_bytes = b->last_staging_used;
		stats.transient_peak_bytes = b->peak_transient_used;
		stats.staging_peak_bytes = b->peak_staging_used;

		VmaTotalStatistics memory_stats = {};
		vmaCalculateStatistics(b->ctx.allocator, &memory_stats);
		stats.device_memory_bytes = memory_stats.total.statistics.allocationBytes;

		for (u32 i = 0; i < b->ctx.in_flight; ++i)
		{
			for (u32 q = 0; q < vk::QUEUE_COUNT; ++q)
			{
				stats.command_lists += opened_count(*b, b->slots[i], q);
			}
		}

		return stats;
	}
}
