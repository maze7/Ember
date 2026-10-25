#pragma once

// Fiber jobs based on Christian Gyrling's 2015 GDC design. Counters join work by parking the
// waiting fiber while its worker runs other jobs. Job data and counters remain caller-owned.
//
// Main resumes on worker 0, the thread that initialized the scheduler. Other fibers may migrate:
// re-read thread-local state after a wait, and never hold a spin lock or thread-owned mutex
// across one. Use JobMutex for contention that should park a fiber.
//
// Blocking operations belong on I/O threads through kick_io(), never on a worker.

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

	// Workers drain higher priorities first. Sustained High traffic can starve lower queues;
	// reserve it for short jobs that unblock the frame.
	enum class JobPriority : u8
	{
		High,
		Normal,
		Low,
		Count
	};

	// data and the profiler/debugger name are borrowed until the job returns.
	struct JobDef
	{
		JobFn fn = nullptr;
		void* data = nullptr;
		const char* name = nullptr;
		JobPriority priority = JobPriority::Normal;
	};

	// Caller-owned completion count. Several kicks may share it, including work kicked by a job
	// already counted here. Keep it alive until the count is zero and every waiter has returned.
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

		// A completed count makes the finished jobs' writes visible. Callable from any thread.
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

	// Spins briefly, then parks a contending scheduler fiber. Threads outside the scheduler
	// keep spinning because they have no fiber to park.
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
		// Includes worker 0. Zero selects hardware_count() minus reserved_threads, at least one.
		u32 worker_count = 0;
		u32 reserved_threads = 1;

		// Pin workers to logical processors instead of leaving placement to the OS.
		bool pin_workers = false;

		// Jobs that may be running or parked at once, and the stack each one gets.
		u32 fiber_count = 160;
		u32 stack_size = 256_kb;

		// Jobs queued per priority.
		u32 queue_capacity = 4096;

		// A wait with no fiber and no progress for this long dumps and fails. Zero never fails.
		u32 stall_report_ms = 1000;

		// I/O threads and queued tasks per priority. Zero threads disables kick_io().
		u32 io_threads = 1;
		u32 io_queue_capacity = 1024;
	};

	// One parallel_for partition: [begin, end) and its index in the split.
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
		// Item count and target items per job. MAX_RANGE_JOBS bounds the split.
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

	// Adds the batch to counter before publishing any job; each return completes one count.
	// Without a counter, the caller must track data lifetime separately. Any thread may kick;
	// a full queue is fatal.
	void kick(Span<const JobDef> jobs, Counter* counter = nullptr);

	inline void kick(const JobDef& job, Counter* counter = nullptr)
	{
		kick(Span<const JobDef>(&job, 1), counter);
	}

	// Parks the calling fiber until zero. External threads poll. Never call from an I/O thread:
	// blocking it can prevent the work needed to complete the counter.
	void wait(Counter& counter);

	// One completion from outside the scheduler. Signal exactly as many times as the pending count;
	// the last one wakes every waiter.
	void signal(Counter& counter);

	// Reasons kick_io() refused a task. Refusal adds no outstanding completion.
	enum class IoKickError : u8
	{
		None = 0,

		// No job system, or one with io_threads = 0.
		NotRunning,

		// Shutdown has begun; nothing more is accepted.
		Stopping,

		// The priority queue cannot accept another task; the caller may retry.
		QueueFull,

		Count
	};

	// Runs blocking work on an I/O thread's stack, in priority order. Accepted work adds one
	// completion, signaled when the task returns. Keep data, name, and completion alive until then.
	// A task may kick jobs, signal counters, or submit more I/O work, but must never wait().
	[[nodiscard]] IoKickError kick_io(const JobDef& job, Counter& completion);

	u32 worker_count();

	// The worker the caller runs on, NO_WORKER on other threads. Stale after a wait: read it again.
	u32 worker_index();

	// True on the fiber that called initialize(): main, which owns the platform layer. False inside
	// any job, worker 0's included, and on threads outside the scheduler.
	bool is_main();

	JobStats stats();

	// Estimates stack high-water use by scanning fill bytes in tracking builds; zero otherwise.
	// Stop stack writers before scanning. Use for diagnostics and sizing, away from hot paths.
	u32 stack_high_water();

	// Logs parked fibers to diagnose hangs. Keep wait state stable: concurrent wakes can invalidate
	// the counters being inspected.
	void dump_state();

	// Borrows a callable without copying it. Keep it alive until completion, for example by
	// kicking and waiting in the same scope.
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

	// Borrows fn(JobRange) until the joined call returns.
	template <typename F>
	void parallel_for(const ParallelForDef& def, F& fn)
	{
		static_assert(std::is_invocable_v<F&, JobRange>, "a range callable takes a JobRange");
		static_assert(!std::is_const_v<F>, "the callable is the job's data and may be mutated");

		parallel_for(def, [](JobRange range, void* data) { (*(F*)data)(range); }, &fn);
	}
}
