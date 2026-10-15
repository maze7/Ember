#pragma once

// The scheduler behind the jobs API. Fiber records and their pool, the wait list, the sleepers,
// the workers and their loop, and the IO threads. Private to the module.

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

// Tracy's fiber support, when the build has it. The profiler then shows each fiber as its own track
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

	// A queued job: the definition minus the priority, which the queue implies, plus the counter it
	// completes. Null for fire and forget jobs.
	struct Job
	{
		JobFn fn = nullptr;
		void* data = nullptr;
		const char* name = nullptr;
		Counter* counter = nullptr;
	};

	// What a parked fiber waits for: a counter to reach zero, or a mutex to come free.
	enum class WaitKind : u8
	{
		None,
		Counter,
		Mutex,
	};

	// A push fails because the queue is full, or because a pop of the target cell is mid flight on
	// another thread and has not released it yet. The hint tells the two apart, and the second
	// clears as soon as the pop finishes.
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

	// The scheduler's side of a fiber: the fiber itself, and what the loop, the wait list, and the
	// dumps need to know about it.
	struct FiberRecord
	{
		Fiber fiber;

		// Written by whoever switches to the fiber, right before the switch.
		Worker* worker = nullptr;

		// What the fiber waits on, from the wait until it returns. park() and the dumps read it;
		// the kind says what the key is.
		std::atomic<const void*> wait_key{nullptr};
		WaitKind wait_kind = WaitKind::None;

		// The link in a wait list bucket.
		FiberRecord* next_waiter = nullptr;

		// The job on this fiber, while one runs, and the fiber's name, which the profiler keys
		// fibers by, so it is persistent.
		const char* job_name = nullptr;
		char name[24] = {};

		// A pool fiber. Thread records are the thread itself to the profiler.
		bool pool = false;
	};

	// The pool of fibers, their records and their stacks. A fiber goes back on the free list when
	// it has nothing left to run, and the list holds every fiber, so a push never fails for room.
	// The stacks are one slice of the stack partition, a guard page then a stack per fiber, and in
	// a tracking build each stack is filled so its high water can be read.
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

	// What the fiber that just switched away needs done once it is safely off its stack. The fiber
	// that receives control does it first thing.
	enum class Pending : u8
	{
		None,

		// Link it into its wait list bucket.
		Park,

		// Return it to the free list.
		Free
	};

	// Where idle workers sleep. A worker with nothing runnable sets its bit in the mask and waits
	// on its own epoch; a producer that finds the bit set claims it and bumps that epoch. The two
	// sides form a Dekker exchange: the sleeper announces, then checks for work, and the procuer
	// publishes work, then checks the mask. Both touch the mask with read-modify-writes, so those
	// form one total order: either the producer sees the bit and wakes the worker, or the work went
	// up before the bit did and the check finds it.
	struct Sleepers
	{
		static constexpr u32 MAX_WORKERS = 64;

		struct alignas(EMBER_CACHE_LINE) Epoch
		{
			std::atomic<u32> value{0};
		};

		alignas(EMBER_CACHE_LINE) std::atomic<u64> mask{0};
		Epoch epochs[MAX_WORKERS];

		// Workers that may sleep, for wake all.
		u32 count = 0;

		// Sleeps the worker until a wake-up, unless busy() finds work after the announcement.
		template <typename Busy>
		void sleep(u32 index, Busy&& busy);

		void wake_one(u32 index);
		void wake_some(u32 wanted);
		void wake_all();
	};

	// The wait list: fibers whose counter was unfinished, or whose mutex was taken, when they
	// looked, linke dunder the address they wait on. Buckets spread the lock. The counter itself is
	// never locked, so a completion is one atomic subtract and a waiter may destroy its counter the
	// instant it reads zero. The last completion finds the waiters by address alone.
	struct WaitList
	{
		static constexpr u32 BUCKET_COUNT = 64;

		struct alignas(EMBER_CACHE_LINE) Bucket
		{
			std::atomic<bool> locked{false};

			// Under the lock.
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

		// True when what the fiber waits for has happened: its counter is zero or its mutex is
		// free. Read under the bucket lock by park() and in the open by a stalled wait.
		static bool satisfied(const FiberRecord* fiber);
	};

	// The IO threads where kick_io() jobs run, outside the normal scheduler. A job here may block
	// as long as it likes and no worker notices. It completes its counter the way every job does,
	// so a fiber parked on a read waks the moment the bytes are in, on whichever worker is free.
	//
	// One queue per priority, drained High first. The semaphore holds one token per queued task: a
	// thread sleeps on it until a submit releases one, and shutdown releases one per thread to let
	// them out.
	struct IoThreads
	{
		// Past this the device is the limit, not the queue.
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

	// One worker thread. Worker 0 is the thread that initialized the job system; the others are
	// created with the system and live until it is shut down.
	struct Worker
	{
		Scheduler* scheduler = nullptr;
		u32 index = 0;

		// The fiber running on this thread.
		FiberRecord* current = nullptr;

		// Taken at construction, handed to the thread where it starts.
		FiberRecord* first = nullptr;

		// The thread's own stack.
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

		// Fibers whose wait completed, and main's, which only worker 0 takes.
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

		// The one fiber with a home thread.
		FiberRecord* main_fiber()
		{
			return &workers[0].thread_record;
		}

		// Counters and mutexes. Both are an atomic word; the fibers waiting on them live in
		// the wait list.
		void add(Counter& counter, u32 count);
		void undo(Counter& counter);
		void complete(Counter& counter);
		void park(FiberRecord* fiber);
		void wake_waiters(const void* key);

		// Fibers.
		FiberRecord* pop_ready(Worker& worker);
		FiberRecord* pop_fiber(Worker& worker);
		void make_ready(FiberRecord* fiber);
		void switch_to(Worker& worker, FiberRecord* from, FiberRecord* to);
		void finish_switch(Worker& worker);
		Worker* yield_to(FiberRecord* self, FiberRecord* next, Pending action);

		// Workers.
		void worker_main(Worker& worker);
		void adopt_main();
		void release_main();
		bool has_work(const Worker& worker) const;
		void idle(Worker& worker);

		// Jobs.
		void kick(Span<const JobDef> jobs, Counter* counter);
		bool pop_job(Job& job);
		void run_job(FiberRecord* self, const Job& job);
		void loop(FiberRecord* self);
		void wait(Counter& counter);
		void lock(JobMutex& mutex);
		void unlock(JobMutex& mutex);
		void park_until_satisfied(FiberRecord* self, Worker* worker);
		FiberRecord* stall(FiberRecord* self);

		// Diagnostics.
		void dump(const char* reason);
		const char* fiber_label(const FiberRecord& fiber) const;
		JobStats stats() const;
		u32 stack_high_water() const;
	};

	// Call boundaries, so that no caller keeps a thread local's address across a switch: the worker
	// this thread is, and whether this thread is an IO thread, which may never wait.
	EMBER_NOINLINE Worker* current_worker();
	EMBER_NOINLINE bool is_io_thread();
	void mark_io_thread();

	// Running out of a pool is a configuration error, so it ends the process the way an allocation
	// failure does, in every build.
	[[noreturn]] void fail(const char* what);
}
