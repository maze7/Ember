#pragma once

// Ember's job system is fiber based and loosely follows the design presented by Christian Gyrling
// in his 2015 GDC talk on paralleliziing the Naughty Dog engine. A job is a function and a pointer.
// Kicking jobs adds them to a Counter the caller owns, finishing a job subtracts one from the
// Counter, and waiting parks the calling fiber until the counter reaches zero while the worker runs
// other jobs.
//
// Waiting on a counter is the only way to synchronize jobs, and a job may wait anywhere, deep in a
// call stack or in the middle of a loop, because its stack its fiber's and nobody else's.
//
// A thread is the execution unit and a fiber is the context. Worker 0 is the thread that called
// initialize(); its own stack is main's fiber, and main always resumes on Worker 0, so the platform
// layer remains on the owning thread. Every other fiber resumes on whichever worker is free, so a
// job must not keep thread-affine state, a thread-local it read before a wait, or a lock a wait.
// System primitives that remember their thread (a mutex or a condition variable) are not allowed
// inside jobs at all: A JobMutex should be used instead.
//
// Work that blocks belongs on an IO thread outside of the scheduler. This way, file, sockets and
// system-call threads never block a worker. IO threads wait on their work and signal a Counter to
// communicate with the job system when their work is complete.

#include <ember/containers/span.h>
#include <ember/core/macros.h>
#include <ember/core/types.h>

#include <atomic>
#include <type_traits>

namespace ember::jobs
{
	using JobFn = void (*)(void* data);

	// Returned by worker_index() on a thread outside the scheduler.
	inline constexpr u32 NO_WORKER = ~0u;

	// Which jobs workers drain first. Priority affects latency, not completion: sustained High
	// traffic delays Normal and Low work. Reserve High for short jobs that unblock the frame, and
	// use Low for work whose delay cannot stall it. There is no stealing between queues.
	enum class JobPriority : u8
	{
		High,
		Normal,
		Low,
		Count
	};

	// Defines a single job. [data] is borrowed and must outlive the job, [name] is for the profiler
	// and the debugger, where the fiber running the job is named after it.
	struct JobDef
	{
		JobFn fn = nullptr;
		void* data = nullptr;
		const char* name = nullptr;
		JobPriority priority = JobPriority::Normal;
	};

	// Work not finished yet, owned by the caller. kick() adds the jobs it kicks, each job subtracts
	// itself when it returns, and wait() returns once the count is zero. Several kicks may share
	// one counter, and a job may kick more work onto the counter its parent waits on.
	class alignas(EMBER_CACHE_LINE) Counter final
	{
	public:
		explicit Counter(u32 pending = 0) : m_remaining(pending)
		{
		}

		~Counter()
		{
			EMBER_ASSERT_MSG(m_remaining.load(std::memory_order_relaxed) == 0,
			    "counter destroyed with work outstanding");
		}

		Counter(const Counter&) = delete;
		Counter& operator=(const Counter&) = delete;

		// True once every completion landed, with the job's writes visible. Any thread.
		bool is_complete() const
		{
			return m_remaining.load(std::memory_order_acquire) == 0;
		}

	private:
		friend struct Scheduler;
		friend struct WaitList;

		std::atomic<u32> m_remaining{0};
	};

	static_assert(sizeof(Counter) == EMBER_CACHE_LINE,
	    "one line per counter, so completions never false share");

	// A lock a job may hold for longer than a spin, for a critical section that computes rather
	// than one that takes a few loads and stores. A job that finds it taken spins briefly, then
	// parks its fiber and lets the worker fun something else, and unlock() wakes it. Any thread may
	// take it; a thread outside the scheduler spins, having no fiber to park.
	class alignas(EMBER_CACHE_LINE) JobMutex final
	{
	public:
		JobMutex() = default;

		JobMutex(const JobMutex&) = delete;
		JobMutex& operator=(const JobMutex&) = delete;

		void lock();
		bool try_lock();
		void unlock();

	private:
		friend struct Scheduler;
		friend struct WaitList;

		std::atomic<u32> m_locked{0};
	};

	struct JobsDef
	{
		// Threads that run jobs, worker 0 included. Zero derives it from the hardware thread count
		// less [reserved_threads], which are left to the system, the IO threads and whatever else
		// the machine runs.
		u32 worker_count = 0;
		u32 reserved_threads = 1;

		// Locks each worker to a core. Off on desktop by default, where the system schedules around
		// other work. Intended for consoles.
		bool pin_workers = false;

		// Jobs that may be running or parked at once, and the stack each one gets.
		u32 fiber_count = 160;
		u32 stack_size = 256_kb;

		// Jobs queued per priority.
		u32 queue_capacity = 4096;

		// A wait with no fiber and no progress for this long dumps and fails. Zero never fails.
		u32 stall_report_ms = 1000;

		// Threads that run submit_io() tasks, and the tasks queued at once per priority. Zero
		// threads disables submit_io(); more let reads overlap on a device that can.
		u32 io_threads = 1;
		u32 io_queue_capacity = 1024;
	};

	// The half-open index range one job of a parallel_for covers, and that job's index in the split
	struct JobRange
	{
		u32 begin = 0;
		u32 end = 0;
		u32 index = 0;

		u32 count() const
		{
			return end - begin;
		}
	};

	using RangeFn = void (*)(JobRange range, void* data);

	// Jobs one parallel_for may split into. The per-job records live on the caller's stack.
	inline constexpr u32 MAX_RANGE_JOBS = 64;

	struct ParallelForDef
	{
		// Items, indexed [0, count), and how many one job is worth, which sets the job count.
		u32 count = 0;
		u32 grain = 1;

		const char* name = nullptr;
		JobPriority priority = JobPriority::Normal;
	};

	// A snapshot for debug views. Every count is approximate while jobs run.
	struct JobStats
	{
		u32 free_fibers = 0;
		u32 parked_fibers = 0;
		u32 ready_fibers = 0;
		u32 queued_jobs = 0;
		u32 queued_io_tasks = 0;

		// Waits that found no free fiber.
		u64 stalls = 0;
	};

	// Creates the fibers and queues and starts the worker and IO threads. The calling thread
	// becomes worker 0. Returns false when the stack partition is too small or a thread could not
	// be started, after logging why.
	bool initialize(const JobsDef& def = {});

	// Joins the workers and the IO threads and releases the fibers and queues. Every job and task
	// must be complete, and this runs on the thread that initialized, outside any job.
	void shutdown();

	// Kicks jobs. With a counter, every job is added to it before any can start and subtracts
	// itself when it finishes. Without one the jobs are fire and forget. Any thread, a job or an
	// IO thread included; a full queue is fatal.
	void kick(Span<const JobDef> jobs, Counter* counter = nullptr);

	inline void kick(const JobDef& job, Counter* counter = nullptr)
	{
		kick(Span<const JobDef>(&job, 1), counter);
	}

	// parks the calling job, or main, until the counter reaches zero. A thread outside the
	// scheduler has no fiber to park, so it polls instead. Never call from an IO thread: nothing
	// would serve its queue.
	void wait(Counter& counter);

	// One completion from outside the scheduler. Signal exactly as many times as the pending count;
	// the last one wakes every waiter.
	void signal(Counter& counter);

	// Why kick_io() refused a task, or None when it did not.
	enum class IoKickError : u8
	{
		None = 0,

		// No job system, or one with io_threads = 0.
		NotRunning,

		// Shutdown has begun; nothing more is accepted.
		Stopping,

		// More tasks in flight than the queue holds; back off and try again.
		QueueFull,

		Count
	};

	// Submit work for the IO thread(s). Designed for work that blocks on the operating system: file
	// reads, a sockets, shader compiles, etc. Anything that would otherwise hold a worker for the
	// duration. It runs on the IO thread's own stack, High priority first the same way workers
	// drain. A task may kick jobs, signal counters and submit more tasks, but never wait: there is
	// no fiber to park.
	[[nodiscard]] IoKickError kick_io(const JobDef& job, Counter& completion);

	u32 worker_count();

	// The worker the caller runs on, NO_WORKER on other threads. Stale after a wait: read it again.
	u32 worker_index();

	// True on the fiber that called initialize(): main, which owns the platform layer. False inside
	// any job, worker 0's included, and on threads outside the scheduler.
	bool is_main();

	JobStats stats();

	// The deepest any fiber's stack has been, in bytes, found by scanning for the fill the stacks
	// were given at initialize(). Only a tracking build knows it, and the scan touches every stack,
	// so this is for a debug view only. The number stack_size is measured against; a fiber that
	// reached its guard page has already faulted.
	u32 stack_high_water();

	// Logs every fiber waiting on a counter or a mutex, for a hang you are looking at. Any thread.
	void dump_state();

	// A job over a callable the caller keeps alive until the job completes: the callable is the
	// job's data and the thunk invokes it. A local lambda kicked and waited for in the same scope
	// is the usual shape. Nothing is copied, so a temporary would dangle.
	template <typename F>
	[[nodiscard]] JobDef make_job(F& fn, const char* name = nullptr)
	{
		static_assert(std::is_invocable_v<F&>, "a job callable takes no arguments");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		return JobDef{
		    .fn = [](void* data) { (*(F*)data)(); },
		    .data = &fn,
		    .name = name,
		};
	}

	// Splits [0, count) into at most MAX_RANGE_JOBS even ranges of about [grain] items, kicks
	// one job per range and waits for all of them. Zero items kicks nothing.
	void parallel_for(const ParallelForDef& def, RangeFn fn, void* data = nullptr);

	// The callable form; fn(JobRange) is the job body and outlives the call by construction.
	template <typename F>
	void parallel_for(const ParallelForDef& def, F& fn)
	{
		static_assert(std::is_invocable_v<F&, JobRange>, "a range callable takes a JobRange");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		parallel_for(def, [](JobRange range, void* data) { (*(F*)data)(range); }, &fn);
	}
}
