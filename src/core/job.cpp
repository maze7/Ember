#include "job_scheduler.h"

#include <ember/core/enum.h>
#include <ember/memory/memory.h>
#include <ember/memory/new.h>

EMBER_ENUM_NAMES(ember::jobs::IoKickError, "None", "NotRunning", "Stopping", "QueueFull");

namespace ember::jobs
{
	namespace
	{
		constinit Scheduler* s_scheduler = nullptr;

		Scheduler& scheduler()
		{
			EMBER_ASSERT_MSG(s_scheduler != nullptr, "no job system");
			return *s_scheduler;
		}
	}

	bool initialize(const JobsDef& def)
	{
		EMBER_ASSERT_MSG(s_scheduler == nullptr, "one job system per process");

		Scheduler* scheduler = memory::new_object<Scheduler>(memory::heap(), memory::Tag::Engine);
		if (!scheduler->initialize(def))
		{
			scheduler->shutdown();
			memory::delete_object(memory::heap(), scheduler);
			return false;
		}

		s_scheduler = scheduler;
		return true;
	}

	void shutdown()
	{
		if (s_scheduler == nullptr)
		{
			return;
		}

		s_scheduler->shutdown();
		memory::delete_object(memory::heap(), s_scheduler);
		s_scheduler = nullptr;
	}

	void kick(Span<const JobDef> jobs, Counter* counter)
	{
		if (jobs.size() != 0)
		{
			scheduler().kick(jobs, counter);
		}
	}

	IoKickError kick_io(const JobDef& job, Counter& completion)
	{
		EMBER_ASSERT(job.fn != nullptr);

		if (s_scheduler == nullptr)
		{
			return IoKickError::NotRunning;
		}

		return s_scheduler->io.kick(job, completion);
	}

	void wait(Counter& counter)
	{
		scheduler().wait(counter);
	}

	void signal(Counter& counter)
	{
		scheduler().complete(counter);
	}

	u32 worker_count()
	{
		return scheduler().worker_count;
	}

	u32 worker_index()
	{
		const Worker* worker = current_worker();
		return worker != nullptr ? worker->index : NO_WORKER;
	}

	bool is_main()
	{
		const Worker* worker = current_worker();
		return worker != nullptr && worker->current == scheduler().main_fiber();
	}

	JobStats stats()
	{
		return scheduler().stats();
	}

	u32 stack_high_water()
	{
		return scheduler().stack_high_water();
	}

	void dump_state()
	{
		scheduler().dump("job system state");
	}

	void JobMutex::lock()
	{
		if (try_lock())
		{
			return;
		}

		scheduler().lock(*this);
	}

	bool JobMutex::try_lock()
	{
		u32 expected = 0;
		return m_locked.compare_exchange_strong(expected, 1, std::memory_order_acquire,
		    std::memory_order_relaxed);
	}

	void JobMutex::unlock()
	{
		EMBER_ASSERT_MSG(m_locked.load(std::memory_order_relaxed) == 1, "unlock of a free mutex");
		m_locked.store(0, std::memory_order_release);

		// Nothing can be parked on it before the job system exists.
		if (s_scheduler != nullptr)
		{
			s_scheduler->unlock(*this);
		}
	}

	namespace
	{
		struct RangeJob
		{
			JobRange range;
			RangeFn fn;
			void* data;
		};

		void run_range(void* data)
		{
			const RangeJob& job = *(const RangeJob*)data;
			job.fn(job.range, job.data);
		}
	}

	void parallel_for(const ParallelForDef& def, RangeFn fn, void* data)
	{
		EMBER_ASSERT(fn != nullptr);

		if (def.count == 0)
		{
			return;
		}

		const u32 grain = def.grain == 0 ? 1 : def.grain;
		const u32 wanted = 1 + (def.count - 1) / grain;
		const u32 jobs = wanted < MAX_RANGE_JOBS ? wanted : MAX_RANGE_JOBS;
		const u32 per_job = def.count / jobs;
		const u32 leftover = def.count % jobs;

		RangeJob ranges[MAX_RANGE_JOBS];
		JobDef defs[MAX_RANGE_JOBS];

		u32 begin = 0;
		for (u32 index = 0; index < jobs; ++index)
		{
			const u32 end = begin + per_job + (index < leftover ? 1 : 0);

			ranges[index] = {.range = {begin, end, index}, .fn = fn, .data = data};
			defs[index] = {.fn = run_range,
			    .data = &ranges[index],
			    .name = def.name,
			    .priority = def.priority};

			begin = end;
		}

		Counter done;
		kick(Span<const JobDef>(defs, jobs), &done);
		wait(done);
	}
}
