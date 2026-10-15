#include "job_scheduler.h"

#include <ember/core/log.h>
#include <ember/core/thread.h>
#include <ember/memory/memory.h>
#include <ember/memory/virtual_memory.h>

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace ember::jobs
{
	namespace
	{
		// The only thread locals in the system, read through the two calls below and nowhere
		// else, so that no caller keeps an address across a switch; see fiber.h.
		constinit thread_local Worker* t_worker = nullptr;
		constinit thread_local bool t_io_thread = false;

		// What the stacks are filled with, for the high water scan.
		constexpr u8 STACK_FILL = 0xFB;

		void fiber_main(void* arg)
		{
			FiberRecord* self = (FiberRecord*)arg;
			self->worker->scheduler->loop(self);
		}

		size_t queue_capacity(u32 count)
		{
			return std::bit_ceil(count < 2 ? 2u : count);
		}

		// Pools never fill: an object is pushed only by the holder that took it out.
		template <typename T>
		void push_held(MpmcQueue<T>& queue, const T& value)
		{
			const bool pushed = push_or_full(queue, value);
			EMBER_ASSERT_MSG(pushed, "a pool queue is sized for every object");
			(void)pushed;
		}

		// A thread's time on a fiber shows in the profiler under the fiber's name, so a job is
		// one track wherever its pieces ran. Thread records are the thread itself.
		void trace_enter(const FiberRecord* fiber)
		{
			if (fiber->pool)
			{
				EMBER_FIBER_ENTER(fiber->name);
			}
		}

		void trace_leave(const FiberRecord* fiber)
		{
			if (fiber->pool)
			{
				EMBER_FIBER_LEAVE();
			}
		}
	}

	Worker* current_worker()
	{
		return t_worker;
	}

	bool is_io_thread()
	{
		return t_io_thread;
	}

	void mark_io_thread()
	{
		t_io_thread = true;
	}

	void fail(const char* what)
	{
		EMBER_ERROR("jobs: {}", what);
		EMBER_DEBUG_BREAK();
		std::abort();
	}

	// Test before the exchange, so a spinner reads the line shared instead of bouncing it.
	void WaitList::Bucket::lock()
	{
		for (u32 spins = 0;; internal::cpu_relax(spins++))
		{
			if (locked.load(std::memory_order_relaxed))
			{
				continue;
			}

			if (!locked.exchange(true, std::memory_order_acquire))
			{
				return;
			}
		}
	}

	void WaitList::Bucket::unlock()
	{
		locked.store(false, std::memory_order_release);
	}

	// Counters and mutexes are cache line aligned, so the line index is the natural key.
	WaitList::Bucket& WaitList::bucket_for(const void* key)
	{
		return buckets[((uintptr_t)key / EMBER_CACHE_LINE) % BUCKET_COUNT];
	}

	bool WaitList::satisfied(const FiberRecord* fiber)
	{
		const void* key = fiber->wait_key.load(std::memory_order_acquire);

		switch (fiber->wait_kind)
		{
			case WaitKind::Counter:
				return ((const Counter*)key)->is_complete();
			case WaitKind::Mutex:
				return ((const JobMutex*)key)->m_locked.load(std::memory_order_acquire) == 0;
			default:
				return true;
		}
	}

	// Under the bucket lock, the completion that satisfies the wait has either happened
	// already, which the check sees, or has not taken the bucket yet, in which case it finds
	// the link. The counter is alive here: its waiter has not returned.
	bool WaitList::park(FiberRecord* fiber)
	{
		const void* key = fiber->wait_key.load(std::memory_order_relaxed);
		Bucket& bucket = bucket_for(key);

		bucket.lock();

		if (satisfied(fiber))
		{
			bucket.unlock();
			return false;
		}

		fiber->next_waiter = bucket.head;
		bucket.head = fiber;

		bucket.unlock();
		return true;
	}

	// Only the address is compared: the counter may already be gone.
	FiberRecord* WaitList::take(const void* key)
	{
		Bucket& bucket = bucket_for(key);
		FiberRecord* taken = nullptr;

		bucket.lock();

		for (FiberRecord** link = &bucket.head; *link != nullptr;)
		{
			FiberRecord* fiber = *link;

			if (fiber->wait_key.load(std::memory_order_relaxed) != key)
			{
				link = &fiber->next_waiter;
				continue;
			}

			*link = fiber->next_waiter;
			fiber->next_waiter = taken;
			taken = fiber;
		}

		bucket.unlock();
		return taken;
	}

	void Scheduler::add(Counter& counter, u32 count)
	{
		counter.m_remaining.fetch_add(count, std::memory_order_relaxed);
	}

	// Takes back one add that never became a job: nothing to wake, since nothing ran.
	void Scheduler::undo(Counter& counter)
	{
		counter.m_remaining.fetch_sub(1, std::memory_order_relaxed);
	}

	// One subtract. The release half publishes the job's writes to whoever reads zero. Past
	// the subtract the counter is nobody's: a waiter that read zero may have destroyed it,
	// so the last completion goes on by address alone.
	void Scheduler::complete(Counter& counter)
	{
		const u32 before = counter.m_remaining.fetch_sub(1, std::memory_order_acq_rel);
		EMBER_ASSERT_MSG(before != 0, "counter completed past zero");

		if (before == 1)
		{
			wake_waiters(&counter);
		}
	}

	void Scheduler::wake_waiters(const void* key)
	{
		FiberRecord* wake = wait_list.take(key);

		while (wake != nullptr)
		{
			FiberRecord* next = wake->next_waiter;
			parked.fetch_sub(1, std::memory_order_relaxed);
			make_ready(wake);
			wake = next;
		}
	}

	// Runs on the fiber that took over from the parking one, so the parked context is
	// complete before anything can resume it.
	void Scheduler::park(FiberRecord* fiber)
	{
		if (wait_list.park(fiber))
		{
			parked.fetch_add(1, std::memory_order_relaxed);
			return;
		}

		make_ready(fiber);
	}

	bool FiberPool::initialize(u32 fiber_count, size_t size, FiberEntry entry)
	{
		EMBER_ASSERT_MSG(records == nullptr, "initialize runs once");

		// A guard page, then the stack, per fiber, from the stack partition. The guard is
		// decommitted, so a fiber that runs off the end of its stack faults there instead of
		// writing over the fiber below.
		const size_t page = virtual_memory::page_size();
		stack_size = align_up(size, page);
		stride = page + stack_size;
		count = fiber_count;

		stacks = memory::allocate_stacks(stride * count);
		if (stacks == nullptr)
		{
			EMBER_ERROR("jobs: the stack partition cannot hold {} fibers of {} bytes each; raise "
			            "MemoryDef::stack_capacity",
			    count, stack_size);
			return false;
		}

		records = (FiberRecord*)memory::allocate(memory::Tag::Engine, count * sizeof(FiberRecord),
		    alignof(FiberRecord));
		free_list.initialize(memory::heap(), memory::Tag::Engine, queue_capacity(count));

		for (u32 index = 0; index < count; ++index)
		{
			FiberRecord& record = *::new ((void*)(records + index)) FiberRecord;
			record.pool = true;
			std::snprintf(record.name, sizeof(record.name), "fiber %u", index);

			u8* guard = stacks + (size_t)index * stride;
			u8* stack = guard + page;
			(void)virtual_memory::decommit(guard, page);

#if EMBER_MEMORY_TRACKING >= 1
			std::memset(stack, STACK_FILL, stack_size);
#endif

			fiber_initialize(record.fiber,
			    {.stack = stack, .stack_size = stack_size, .entry = entry, .arg = &record});
			push_free(&record);
		}

		return true;
	}

	void FiberPool::shutdown()
	{
		if (records == nullptr)
		{
			return;
		}

		for (u32 index = 0; index < count; ++index)
		{
			fiber_destroy(records[index].fiber);
			records[index].~FiberRecord();
		}

		memory::free(records);
		free_list.shutdown();

		// The stacks stay with the partition.
		records = nullptr;
		stacks = nullptr;
		count = 0;
	}

	FiberRecord* FiberPool::pop_free()
	{
		FiberRecord* fiber = nullptr;
		return free_list.try_pop(fiber) ? fiber : nullptr;
	}

	void FiberPool::push_free(FiberRecord* fiber)
	{
		push_held(free_list, fiber);
	}

	u32 FiberPool::free_count() const
	{
		return (u32)free_list.size_hint();
	}

	u8* FiberPool::stack_of(u32 index) const
	{
		return stacks + (size_t)index * stride + virtual_memory::page_size();
	}

	bool Scheduler::initialize(const JobsDef& jobs_def)
	{
		def = jobs_def;

		const u32 hardware = thread::hardware_count();
		const u32 reserved = def.reserved_threads < hardware ? def.reserved_threads : hardware - 1;
		worker_count = def.worker_count != 0 ? def.worker_count : hardware - reserved;
		if (worker_count == 0)
		{
			worker_count = 1;
		}

		// Workers and I/O threads share the arena's thread slots, which the sleeper mask
		// matches in size, so the I/O threads come out of the workers' allowance.
		const u32 io_threads =
		    def.io_threads < IoThreads::MAX_THREADS ? def.io_threads : IoThreads::MAX_THREADS;
		if (worker_count > Sleepers::MAX_WORKERS - io_threads)
		{
			worker_count = Sleepers::MAX_WORKERS - io_threads;
		}

		sleepers.count = worker_count;

		// Every worker thread takes a fiber to run its loop on, and main's first wait needs one
		// more.
		const u32 fiber_count =
		    def.fiber_count > worker_count + 1 ? def.fiber_count : worker_count + 1;
		if (!fibers.initialize(fiber_count, def.stack_size, fiber_main))
		{
			return false;
		}

		ready.initialize(memory::heap(), memory::Tag::Engine, queue_capacity(fibers.count));
		for (MpmcQueue<Job>& queue : jobs)
		{
			queue.initialize(memory::heap(), memory::Tag::Engine,
			    queue_capacity(def.queue_capacity));
		}

		workers = (Worker*)memory::allocate(memory::Tag::Engine, worker_count * sizeof(Worker),
		    alignof(Worker));
		for (u32 index = 0; index < worker_count; ++index)
		{
			Worker& worker = *::new ((void*)(workers + index)) Worker;
			worker.scheduler = this;
			worker.index = index;
		}

		// Threads start last, once every queue and fiber they may touch exists. Each thread's
		// first fiber is taken here, ahead of a main that may drain the pool before the thread
		// gets to run.
		for (u32 index = 1; index < worker_count; ++index)
		{
			Worker& worker = workers[index];
			worker.first = fibers.pop_free();
			EMBER_ASSERT_MSG(worker.first != nullptr, "out of fibers before the workers started");

			worker.thread = std::thread([this, index] { worker_main(workers[index]); });
		}

		io.initialize(*this, io_threads, def.io_queue_capacity);
		adopt_main();
		return true;
	}

	void Scheduler::shutdown()
	{
		if (workers == nullptr)
		{
			fibers.shutdown();
			return;
		}

		// Releasing the adoption undoes a conversion only this thread can undo, and a job on
		// worker 0 would be pulling the scheduler out from under its own fiber.
		EMBER_ASSERT_MSG(t_worker == &workers[0] && workers[0].current == &workers[0].thread_record,
		    "shutdown runs on the thread that initialized, outside any job");

		// The I/O threads first: once they are gone, no completion can arrive from outside the
		// workers.
		io.shutdown();

		stopping.store(true, std::memory_order_release);
		sleepers.wake_all();

		for (u32 index = 1; index < worker_count; ++index)
		{
			if (workers[index].thread.joinable())
			{
				workers[index].thread.join();
			}
		}

		const JobStats snapshot = stats();
		EMBER_ASSERT_MSG(snapshot.free_fibers == fibers.count,
		    "fibers still parked or running at shutdown");
		EMBER_ASSERT_MSG(snapshot.ready_fibers == 0, "fibers still ready at shutdown");
		EMBER_ASSERT_MSG(snapshot.queued_jobs == 0, "jobs still queued at shutdown");
		(void)snapshot;

		release_main();

		for (u32 index = 0; index < worker_count; ++index)
		{
			workers[index].~Worker();
		}

		memory::free(workers);
		workers = nullptr;

		fibers.shutdown();
		ready.shutdown();
		for (MpmcQueue<Job>& queue : jobs)
		{
			queue.shutdown();
		}
	}

	FiberRecord* Scheduler::pop_ready(Worker& worker)
	{
		// Main is worker 0's alone, and it goes first: the frame is usually waiting on it.
		if (worker.index == 0 && main_ready.load(std::memory_order_relaxed) != nullptr)
		{
			if (FiberRecord* main = main_ready.exchange(nullptr, std::memory_order_acquire))
			{
				return main;
			}
		}

		FiberRecord* fiber = nullptr;
		return ready.try_pop(fiber) ? fiber : nullptr;
	}

	// Anything this worker may continue on: a fiber whose wait completed, else a fresh one.
	FiberRecord* Scheduler::pop_fiber(Worker& worker)
	{
		FiberRecord* next = pop_ready(worker);
		return next != nullptr ? next : fibers.pop_free();
	}

	void Scheduler::make_ready(FiberRecord* fiber)
	{
		if (fiber == main_fiber())
		{
			FiberRecord* previous = main_ready.exchange(fiber, std::memory_order_release);
			EMBER_ASSERT_MSG(previous == nullptr, "main made ready twice");
			(void)previous;
			sleepers.wake_one(0);
			return;
		}

		push_held(ready, fiber);
		sleepers.wake_some(1);
	}

	// Returns when from is resumed, on whatever thread resumes it.
	void Scheduler::switch_to(Worker& worker, FiberRecord* from, FiberRecord* to)
	{
		EMBER_ASSERT(from != to);
		to->worker = &worker;
		worker.current = to;

		trace_leave(from);
		fiber_switch(from->fiber, to->fiber);
		trace_enter(from);
	}

	void Scheduler::finish_switch(Worker& worker)
	{
		FiberRecord* fiber = worker.pending_fiber;
		const Pending action = worker.pending;

		worker.pending = Pending::None;
		worker.pending_fiber = nullptr;

		if (action == Pending::Park)
		{
			park(fiber);
		}
		else if (action == Pending::Free)
		{
			fibers.push_free(fiber);
		}
	}

	// Switches to next with an action for whoever takes over, and once something switches
	// back finishes the action that fiber left. Returns the worker this fiber resumed on.
	Worker* Scheduler::yield_to(FiberRecord* self, FiberRecord* next, Pending action)
	{
		Worker& worker = *self->worker;
		worker.pending = action;
		worker.pending_fiber = self;
		switch_to(worker, self, next);

		Worker* resumed = self->worker;
		finish_switch(*resumed);
		return resumed;
	}

	// Worker threads live here. The thread's own stack becomes its thread record, used only
	// to leave the loop at shutdown; everything else runs on pool fibers.
	void Scheduler::worker_main(Worker& worker)
	{
		char name[16];
		std::snprintf(name, sizeof(name), "ember.jobs.%u", worker.index);
		thread::set_name(name);

		if (!memory::initialize_thread())
		{
			fail("no arena slot left for a worker thread");
		}

		t_worker = &worker;

		if (def.pin_workers)
		{
			(void)thread::set_affinity(worker.index % thread::hardware_count());
		}

		fiber_adopt_thread(worker.thread_record.fiber);
		worker.thread_record.worker = &worker;
		worker.current = &worker.thread_record;

		FiberRecord* first = worker.first;
		worker.first = nullptr;

		switch_to(worker, &worker.thread_record, first);

		finish_switch(worker);
		fiber_release_thread(worker.thread_record.fiber);
		t_worker = nullptr;
		memory::shutdown_thread();
	}

	bool Scheduler::has_work(const Worker& worker) const
	{
		if (worker.index == 0 && main_ready.load(std::memory_order_relaxed) != nullptr)
		{
			return true;
		}

		if (ready.size_hint() != 0)
		{
			return true;
		}

		for (const MpmcQueue<Job>& queue : jobs)
		{
			if (queue.size_hint() != 0)
			{
				return true;
			}
		}

		return false;
	}

	// The epoch is read before the announcement, so a wake-up landing between the announcement
	// and the wait shows up as a changed value and the wait returns at once.
	template <typename Busy>
	void Sleepers::sleep(u32 index, Busy&& busy)
	{
		const u32 observed = epochs[index].value.load(std::memory_order_acquire);
		const u64 bit = u64{1} << index;

		mask.fetch_or(bit, std::memory_order_acq_rel);

		if (!busy())
		{
			epochs[index].value.wait(observed, std::memory_order_acquire);
		}

		mask.fetch_and(~bit, std::memory_order_relaxed);
	}

	// Producers touch the mask with a read-modify-write rather than a load: that orders it
	// after the work they just published, against the sleeper's own read-modify-write. The
	// clear doubles as the claim: only the producer that saw the bit wakes the worker.
	void Sleepers::wake_one(u32 index)
	{
		const u64 bit = u64{1} << index;

		if ((mask.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
		{
			return;
		}

		epochs[index].value.fetch_add(1, std::memory_order_release);
		epochs[index].value.notify_one();
	}

	// Claims sleepers one bit at a time, so that several producers do not all wake the same
	// one.
	void Sleepers::wake_some(u32 wanted)
	{
		u64 sleeping = mask.fetch_or(0, std::memory_order_acq_rel);

		while (sleeping != 0 && wanted != 0)
		{
			const u32 index = (u32)std::countr_zero(sleeping);
			const u64 bit = u64{1} << index;
			sleeping &= ~bit;

			if ((mask.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0)
			{
				continue;
			}

			epochs[index].value.fetch_add(1, std::memory_order_release);
			epochs[index].value.notify_one();
			--wanted;
		}
	}

	void Sleepers::wake_all()
	{
		for (u32 index = 0; index < count; ++index)
		{
			epochs[index].value.fetch_add(1, std::memory_order_release);
			epochs[index].value.notify_all();
		}
	}

	// Spin a little for work that is about to arrive, then sleep.
	void Scheduler::idle(Worker& worker)
	{
		for (u32 spin = 0; spin < IDLE_SPINS; ++spin)
		{
			if (has_work(worker) || stopping.load(std::memory_order_relaxed))
			{
				return;
			}

			internal::cpu_relax(spin);
		}

		const auto busy = [&]()
		{
			return has_work(worker) || stopping.load(std::memory_order_acquire);
		};
		sleepers.sleep(worker.index, busy);
	}

	void Scheduler::kick(Span<const JobDef> decls, Counter* counter)
	{
		// Counted before any job can start, so no completion can run the count below zero.
		if (counter != nullptr)
		{
			add(*counter, (u32)decls.size());
		}

		for (const JobDef& decl : decls)
		{
			EMBER_ASSERT(decl.fn != nullptr);

			const Job job{.fn = decl.fn, .data = decl.data, .name = decl.name, .counter = counter};
			if (!push_or_full(jobs[(u32)decl.priority], job))
			{
				fail("job queue full: raise JobsDef::queue_capacity");
			}
		}

		sleepers.wake_some((u32)decls.size());
	}

	bool Scheduler::pop_job(Job& job)
	{
		for (MpmcQueue<Job>& queue : jobs)
		{
			if (queue.try_pop(job))
			{
				return true;
			}
		}

		return false;
	}

	void Scheduler::run_job(FiberRecord* self, const Job& job)
	{
		self->job_name = job.name != nullptr ? job.name : "job";

		{
			EMBER_PROFILE_SCOPE("job");
			EMBER_PROFILE_ZONE_NAME(self->job_name, std::strlen(self->job_name));
			job.fn(job.data);
		}

		self->job_name = nullptr;

		if (job.counter != nullptr)
		{
			complete(*job.counter);
		}
	}

	// Every pool fiber runs this forever. Ready fibers come first because they hold a stack
	// and an unfinished job; new jobs start only when nothing is waiting to resume. A fiber
	// that switches to another one has nothing left to do and frees itself in the process.
	void Scheduler::loop(FiberRecord* self)
	{
		Worker* worker = self->worker;
		trace_enter(self);
		finish_switch(*worker);

		for (;;)
		{
			if (FiberRecord* next = pop_ready(*worker))
			{
				worker = yield_to(self, next, Pending::Free);
				continue;
			}

			Job job;
			if (pop_job(job))
			{
				run_job(self, job);

				// A wait inside the job may have moved the fiber.
				worker = self->worker;
				continue;
			}

			if (stopping.load(std::memory_order_acquire))
			{
				yield_to(self, &worker->thread_record, Pending::Free);
				std::abort();
			}

			idle(*worker);
		}
	}

	// The shared tail of wait() and lock(): park until what the fiber waits for has happened.
	// A wake-up is a hint, as with a futex: a completion that zeroed an earlier counter at this
	// address may wake a fiber parked on this one, which simply parks again.
	void Scheduler::park_until_satisfied(FiberRecord* self, Worker* worker)
	{
		while (!WaitList::satisfied(self))
		{
			FiberRecord* next = pop_fiber(*worker);
			if (next == nullptr)
			{
				next = stall(self);
			}

			if (next == nullptr)
			{
				break;
			}

			worker = yield_to(self, next, Pending::Park);
		}
	}

	void Scheduler::wait(Counter& counter)
	{
		EMBER_ASSERT_MSG(!is_io_thread(),
		    "an I/O thread may not wait on the scheduler: nothing would serve its queue");

		Worker* worker = current_worker();
		if (worker == nullptr)
		{
			// A thread outside the scheduler has no fiber to park, so it polls.
			for (u32 spins = 0; !counter.is_complete(); internal::cpu_relax(spins++))
			{
			}

			return;
		}

		FiberRecord* self = worker->current;
		EMBER_ASSERT_MSG(self->pool || self == main_fiber(), "only jobs and main can wait");

		self->wait_kind = WaitKind::Counter;
		self->wait_key.store(&counter, std::memory_order_release);

		park_until_satisfied(self, worker);

		self->wait_key.store(nullptr, std::memory_order_relaxed);
		self->wait_kind = WaitKind::None;
	}

	// Most holds are short, so spin first. A job then parks on the mutex's address and is
	// woken by unlock(); a thread outside the scheduler keeps spinning, having no fiber.
	void Scheduler::lock(JobMutex& mutex)
	{
		for (u32 spin = 0; spin < LOCK_SPINS; ++spin)
		{
			if (mutex.try_lock())
			{
				return;
			}

			internal::cpu_relax(spin);
		}

		Worker* worker = current_worker();
		if (worker == nullptr)
		{
			for (u32 spins = 0; !mutex.try_lock(); internal::cpu_relax(spins++))
			{
			}

			return;
		}

		FiberRecord* self = worker->current;
		EMBER_ASSERT_MSG(self->pool || self == main_fiber(), "only jobs and main can park");

		self->wait_kind = WaitKind::Mutex;
		self->wait_key.store(&mutex, std::memory_order_release);

		// Woken means the mutex was free a moment ago, not that it is ours: another thread
		// may have taken it since, and then the fiber parks again.
		while (!mutex.try_lock())
		{
			park_until_satisfied(self, worker);
			worker = self->worker;
		}

		self->wait_key.store(nullptr, std::memory_order_relaxed);
		self->wait_kind = WaitKind::None;
	}

	void Scheduler::unlock(JobMutex& mutex)
	{
		wake_waiters(&mutex);
	}

	// Every fiber is parked or busy on another worker. The wait spins for one to come back or
	// for its own condition to be met. Past stall_report_ms of neither, the pool is too small
	// for the job graph or the graph has a cycle: the first worker to notice dumps and fails,
	// the others keep spinning for the moment the failure takes.
	FiberRecord* Scheduler::stall(FiberRecord* self)
	{
		EMBER_PROFILE_SCOPE_C("stalled wait", PROFILE_COLOR_WAIT);
		stalls.fetch_add(1, std::memory_order_relaxed);

		Worker& worker = *self->worker;
		const auto since = std::chrono::steady_clock::now();

		for (u32 spin = 0;; internal::cpu_relax(spin++))
		{
			if (WaitList::satisfied(self))
			{
				return nullptr;
			}

			if (FiberRecord* next = pop_fiber(worker))
			{
				return next;
			}

			if (def.stall_report_ms != 0 && (spin & 1023) == 1023 &&
			    std::chrono::steady_clock::now() - since >=
			        std::chrono::milliseconds(def.stall_report_ms) &&
			    !failing.exchange(true, std::memory_order_acq_rel))
			{
				dump("a wait found no fiber and nothing finished");
				fail("a wait found no fiber and nothing finished");
			}
		}
	}

	// Racy by design: a fiber may wake and its counter go away while this runs. It is a
	// diagnostic for a hang, when nothing is moving.
	void Scheduler::dump(const char* reason)
	{
		const JobStats snapshot = stats();
		EMBER_ERROR("jobs: {}: {} fibers free, {} parked, {} ready, {} jobs queued, {} io tasks "
		            "queued",
		    reason, snapshot.free_fibers, snapshot.parked_fibers, snapshot.ready_fibers,
		    snapshot.queued_jobs, snapshot.queued_io_tasks);

		const auto describe = [this](const FiberRecord& fiber)
		{
			const void* key = fiber.wait_key.load(std::memory_order_acquire);
			if (key == nullptr)
			{
				return;
			}

			if (fiber.wait_kind == WaitKind::Mutex)
			{
				EMBER_ERROR("jobs:   {} waits on mutex {}", fiber_label(fiber), key);
			}
			else
			{
				EMBER_ERROR("jobs:   {} waits on counter {} with {} left", fiber_label(fiber), key,
				    ((const Counter*)key)->m_remaining.load(std::memory_order_relaxed));
			}
		};

		for (u32 index = 0; index < fibers.count; ++index)
		{
			describe(fibers.records[index]);
		}

		describe(*main_fiber());
	}

	const char* Scheduler::fiber_label(const FiberRecord& fiber) const
	{
		if (&fiber == &workers[0].thread_record)
		{
			return "main";
		}

		return fiber.job_name != nullptr ? fiber.job_name : fiber.name;
	}

	JobStats Scheduler::stats() const
	{
		size_t queued = 0;
		for (const MpmcQueue<Job>& queue : jobs)
		{
			queued += queue.size_hint();
		}

		const u32 main_waiting = main_ready.load(std::memory_order_relaxed) != nullptr ? 1u : 0u;

		return JobStats{
		    .free_fibers = fibers.free_count(),
		    .parked_fibers = parked.load(std::memory_order_relaxed),
		    .ready_fibers = (u32)ready.size_hint() + main_waiting,
		    .queued_jobs = (u32)queued,
		    .queued_io_tasks = io.queued(),
		    .stalls = stalls.load(std::memory_order_relaxed),
		};
	}

	// Scans each stack up from its bottom a cache line at a time for the first line that is
	// not all fill: that is as deep as the fiber ever reached.
	u32 Scheduler::stack_high_water() const
	{
#if EMBER_MEMORY_TRACKING >= 1
		u8 fill[EMBER_CACHE_LINE];
		std::memset(fill, STACK_FILL, sizeof(fill));

		u32 deepest = 0;
		for (u32 index = 0; index < fibers.count; ++index)
		{
			const u8* stack = fibers.stack_of(index);
			const u8* top = stack + fibers.stack_size;

			const u8* line = stack;
			while (line < top && std::memcmp(line, fill, sizeof(fill)) == 0)
			{
				line += sizeof(fill);
			}

			const u32 used = (u32)(top - line);
			if (used > deepest)
			{
				deepest = used;
			}
		}

		return deepest;
#else
		return 0;
#endif
	}

	// The thread that creates the scheduler is worker 0. Its own stack becomes main's fiber,
	// so main waits like a job does, and main always resumes on this thread: it owns the
	// platform and the device. Runs at the end of initialize(), on that thread.
	void Scheduler::adopt_main()
	{
		Worker& worker = workers[0];
		fiber_adopt_thread(worker.thread_record.fiber);
		worker.thread_record.worker = &worker;
		worker.current = &worker.thread_record;
		t_worker = &worker;
	}

	void Scheduler::release_main()
	{
		Worker& worker = workers[0];
		t_worker = nullptr;
		fiber_release_thread(worker.thread_record.fiber);
	}
}
