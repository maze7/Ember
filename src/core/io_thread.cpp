#include "job_scheduler.h"

#include <ember/core/profile.h>
#include <ember/core/thread.h>
#include <ember/memory/memory.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <new>

namespace ember::jobs
{
	void IoThreads::initialize(Scheduler& owner, u32 thread_count, u32 queue_capacity)
	{
		EMBER_ASSERT_MSG(threads == nullptr, "initialize runs once");

		scheduler = &owner;

		for (MpmcQueue<Job>& queue : queues)
		{
			queue.initialize(memory::heap(), memory::Tag::Engine,
			    std::bit_ceil(queue_capacity < 2 ? 2u : queue_capacity));
		}

		count = thread_count;
		if (count == 0)
		{
			return;
		}

		threads = (std::thread*)memory::allocate(memory::Tag::Engine, count * sizeof(std::thread),
		    alignof(std::thread));
		for (u32 index = 0; index < count; ++index)
		{
			::new ((void*)(threads + index)) std::thread([this, index] { run(index); });
		}
	}

	void IoThreads::shutdown()
	{
		if (threads != nullptr)
		{
			// One token per thread: each takes one, finds nothing to run, and leaves.
			stopping.store(true, std::memory_order_release);
			tokens.release((ptrdiff_t)count);

			for (u32 index = 0; index < count; ++index)
			{
				threads[index].join();
				threads[index].~thread();
			}

			memory::free(threads);
			threads = nullptr;
		}

		EMBER_ASSERT_MSG(queued() == 0, "io tasks still queued at shutdown");

		for (MpmcQueue<Job>& queue : queues)
		{
			queue.shutdown();
		}

		count = 0;
	}

	IoKickError IoThreads::kick(const JobDef& job, Counter& completion)
	{
		if (threads == nullptr)
		{
			return IoKickError::NotRunning;
		}

		if (stopping.load(std::memory_order_acquire))
		{
			return IoKickError::Stopping;
		}

		// Increment before publishing the task; it may complete as soon as the queue exposes it.
		// Undo the increment if submission fails.
		scheduler->add(completion, 1);

		const Job queued{.fn = job.fn, .data = job.data, .name = job.name, .counter = &completion};
		if (!push_or_full(queues[(u32)job.priority], queued))
		{
			scheduler->complete(completion);
			return IoKickError::QueueFull;
		}

		tokens.release();
		return IoKickError::None;
	}

	u32 IoThreads::queued() const
	{
		size_t total = 0;
		for (const MpmcQueue<Job>& queue : queues)
		{
			total += queue.size_hint();
		}

		return (u32)total;
	}

	void IoThreads::run(u32 index)
	{
		char name[16];
		std::snprintf(name, sizeof(name), "ember.io.%u", index);
		thread::set_name(name);

		if (!memory::initialize_thread())
		{
			fail("no arena slot left for an I/O thread");
		}

		mark_io_thread();

		for (;;)
		{
			tokens.acquire();

			Job job;
			if (!take(job))
			{
				if (stopping.load(std::memory_order_acquire))
				{
					break;
				}

				continue;
			}

			{
				EMBER_PROFILE_SCOPE_C("io task", PROFILE_COLOR_IO);
				const char* label = job.name != nullptr ? job.name : "io task";
				EMBER_PROFILE_ZONE_NAME(label, std::strlen(label));
				(void)label;
				job.fn(job.data);
			}

			if (job.counter != nullptr)
			{
				scheduler->complete(*job.counter);
			}
		}

		memory::shutdown_thread();
	}

	bool IoThreads::take(Job& job)
	{
		for (MpmcQueue<Job>& queue : queues)
		{
			if (queue.try_pop(job))
			{
				return true;
			}
		}

		return false;
	}
}
