#pragma once

// Scheduler internals. Pools are fixed at initialization; waits park fibers while workers
// continue running jobs. Blocking tasks use separate I/O threads.

#include <ember/containers/mpmc_queue.h>
#include <ember/core/jobs.h>
#include <ember/core/macros.h>
#include <ember/core/profile.h>
#include <ember/core/spin_mutex.h>
#include <ember/core/types.h>

#include "fiber.h"

#include <atomic>
#include <semaphore>
#include <thread>

// Keep a fiber on one profiler track as it migrates between workers.
#if defined(TRACY_ENABLE) && defined(TRACY_FIBERS)
	#define EMBER_FIBER_ENTER(name) TracyFiberEnter(name)
	#define EMBER_FIBER_LEAVE() TracyFiberLeave
#else
	#define EMBER_FIBER_ENTER(name) ((void)0)
	#define EMBER_FIBER_LEAVE() ((void)0)
#endif

namespace ember::jobs
{
	struct Worker;
	struct Scheduler;

	// Priority is implied by the queue. A null counter means no completion is tracked.
	struct Job
	{
		JobFn fn = nullptr;
		void* data = nullptr;
		const char* name = nullptr;
		Counter* counter = nullptr;
	};

	enum class WaitKind : u8
	{
		None,
		Counter,
		Mutex,
	};

	// A failed push may mean a consumer has not yet released the target cell. Retry while the
	// size hint leaves room, and report failure when the queue appears full.
	template <typename T>
	[[nodiscard]] bool push_or_full(MpmcQueue<T>& queue, const T& value)
	{
		for (u32 spins = 0;; internal::cpu_relax(spins++))
		{
			if (queue.try_push(value))
			{
				return true;
			}

			if (queue.size_hint() >= queue.capacity())
			{
				return false;
			}
		}
	}

	struct FiberRecord
	{
		Fiber fiber;

		// Written by whoever switches to the fiber, right before the switch.
		Worker* worker = nullptr;

		// Published before parking and cleared when the wait returns. The key's type is wait_kind.
		std::atomic<const void*> wait_key{nullptr};
		WaitKind wait_kind = WaitKind::None;

		FiberRecord* next_waiter = nullptr;

		// Job labels are borrowed while running. Fiber names must persist for profiler identity.
		const char* job_name = nullptr;
		char name[24] = {};

		// A pool fiber. Thread records are the thread itself to the profiler.
		bool pool = false;
	};

	// Records and stacks live until shutdown. The free list holds every pool fiber, so returning
	// one cannot exhaust capacity. Each stack has a guard page and optional high-water fill.
	struct FiberPool
	{
		FiberRecord* records = nullptr;
		u32 count = 0;
		MpmcQueue<FiberRecord*> free_list;

		u8* stacks = nullptr;
		size_t stride = 0;
		size_t stack_size = 0;

		[[nodiscard]] bool initialize(u32 fiber_count, size_t stack_size, FiberEntry entry);
		void shutdown();

		FiberRecord* pop_free();
		void push_free(FiberRecord* fiber);
		u32 free_count() const;

		// The stack of pool fiber [index], lowest address first, above its guard page.
		u8* stack_of(u32 index) const;
	};

	// Defer parking or recycling until the receiving fiber has left the old stack safely.
	enum class Pending : u8
	{
		None,

		Park,

		Free
	};

	// Workers announce sleep before checking for work; producers publish work before checking
	// the sleep mask. Ordered read-modify-writes ensure either the producer wakes the worker or
	// the worker sees the work. Per-worker epochs prevent a wake before wait() from being lost.
	struct Sleepers
	{
		static constexpr u32 MAX_WORKERS = 64;

		struct alignas(EMBER_CACHE_LINE) Epoch
		{
			std::atomic<u32> value{0};
		};

		alignas(EMBER_CACHE_LINE) std::atomic<u64> mask{0};
		Epoch epochs[MAX_WORKERS];

		u32 count = 0;

		// Sleeps the worker until a wake-up, unless busy() finds work after the announcement.
		template <typename Busy>
		void sleep(u32 index, Busy&& busy);

		void wake_one(u32 index);
		void wake_some(u32 wanted);
		void wake_all();
	};

	// Waiters are bucketed by counter or mutex address. Bucket locks serialize registration
	// with wake-up; counters themselves remain lock-free. Once a waiter observes zero it may
	// destroy the counter, so the last completion must find waiters without dereferencing it.
	struct WaitList
	{
		static constexpr u32 BUCKET_COUNT = 64;

		struct alignas(EMBER_CACHE_LINE) Bucket
		{
			std::atomic<bool> locked{false};

			// Protected by locked.
			FiberRecord* head = nullptr;

			void lock();
			void unlock();
		};

		Bucket buckets[BUCKET_COUNT];

		Bucket& bucket_for(const void* key);

		// Links the fiber under its key, or returns false when what it waits for has already
		// happened.
		[[nodiscard]] bool park(FiberRecord* fiber);

		// Unlinks every fiber waiting on the key, chained through next_waiter.
		[[nodiscard]] FiberRecord* take(const void* key);

		// Checks the condition without acquiring a free mutex. Used under the bucket lock during
		// registration and without it while polling a stalled wait.
		static bool satisfied(const FiberRecord* fiber);
	};

	// Blocking tasks run outside the fiber workers and signal normal job counters on completion.
	// One semaphore token admits one task, with High priority drained first. Shutdown releases
	// an extra token per thread so idle threads can exit.
	struct IoThreads
	{
		static constexpr u32 MAX_THREADS = 8;
		static constexpr u32 PRIORITY_COUNT = (u32)JobPriority::Count;

		Scheduler* scheduler = nullptr;
		std::counting_semaphore<> tokens{0};
		std::atomic<bool> stopping{false};

		std::thread* threads = nullptr;
		u32 count = 0;

		MpmcQueue<Job> queues[PRIORITY_COUNT];

		void initialize(Scheduler& owner, u32 thread_count, u32 queue_capacity);

		// Joins the threads. Every submitted task must be complete.
		void shutdown();

		[[nodiscard]] IoKickError kick(const JobDef& job, Counter& completion);
		u32 queued() const;

		void run(u32 index);
		bool take(Job& job);
	};

	// Worker 0 borrows the initializing thread. Other workers own threads until shutdown.
	struct Worker
	{
		Scheduler* scheduler = nullptr;
		u32 index = 0;

		FiberRecord* current = nullptr;

		// Reserved before thread startup so main cannot exhaust the fiber pool first.
		FiberRecord* first = nullptr;

		// Saves the OS thread stack for return at shutdown.
		FiberRecord thread_record;

		Pending pending = Pending::None;
		FiberRecord* pending_fiber = nullptr;

		// Empty for worker 0.
		std::thread thread;
	};

	struct Scheduler
	{
		static constexpr u32 PRIORITY_COUNT = (u32)JobPriority::Count;
		static constexpr u32 IDLE_SPINS = 128;
		static constexpr u32 LOCK_SPINS = 256;

		JobsDef def;
		std::atomic<bool> stopping{false};

		FiberPool fibers;

		// Main uses a separate ready slot because it must resume on worker 0.
		MpmcQueue<FiberRecord*> ready;
		std::atomic<FiberRecord*> main_ready{nullptr};

		Worker* workers = nullptr;
		u32 worker_count = 0;
		Sleepers sleepers;
		IoThreads io;

		MpmcQueue<Job> jobs[PRIORITY_COUNT];

		WaitList wait_list;

		// Fibers linked into the wait list, waits that found no fiber, and whether a stalled
		// worker is already dumping and failing.
		std::atomic<u32> parked{0};
		std::atomic<u64> stalls{0};
		std::atomic<bool> failing{false};

		[[nodiscard]] bool initialize(const JobsDef& def);
		void shutdown();

		FiberRecord* main_fiber()
		{
			return &workers[0].thread_record;
		}

		void add(Counter& counter, u32 count);
		void complete(Counter& counter);
		void park(FiberRecord* fiber);
		void wake_waiters(const void* key);

		FiberRecord* pop_ready(Worker& worker);
		FiberRecord* pop_fiber(Worker& worker);
		void make_ready(FiberRecord* fiber);
		void switch_to(Worker& worker, FiberRecord* from, FiberRecord* to);
		void finish_switch(Worker& worker);
		Worker* yield_to(FiberRecord* self, FiberRecord* next, Pending action);

		void worker_main(Worker& worker);
		void adopt_main();
		void release_main();
		bool has_work(const Worker& worker) const;
		void idle(Worker& worker);

		void kick(Span<const JobDef> jobs, Counter* counter);
		bool pop_job(Job& job);
		void run_job(FiberRecord* self, const Job& job);
		void loop(FiberRecord* self);
		void wait(Counter& counter);
		void lock(JobMutex& mutex);
		void unlock(JobMutex& mutex);
		void park_until_satisfied(FiberRecord* self, Worker* worker);
		FiberRecord* stall(FiberRecord* self);

		void dump(const char* reason);
		const char* fiber_label(const FiberRecord& fiber) const;
		JobStats stats() const;
		u32 stack_high_water() const;
	};

	// Keep TLS reads behind non-inlined calls: a fiber may resume on another thread, and callers
	// must not reuse the old thread's TLS address. I/O threads cannot wait on the scheduler.
	EMBER_NOINLINE Worker* current_worker();
	EMBER_NOINLINE bool is_io_thread();
	void mark_io_thread();

	// Pool exhaustion is a fatal configuration error in every build.
	[[noreturn]] void fail(const char* what);
}
