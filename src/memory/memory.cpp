#include <ember/memory/memory.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ember::memory
{
	namespace
	{
		constinit TaggedHeap s_tagged_heap;
		constinit std::atomic<bool> s_initialized{false};
	}

	bool initialize(const MemoryDef& def)
	{
		bool expected = false;
		if (!s_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		{
			EMBER_ASSERT_MSG(false, "memory is initialized once");
			return false;
		}

		internal::heap_initialize();

		if (!s_tagged_heap.initialize(def.block_heap_capacity, def.block_size))
		{
			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		if (!initialize_thread())
		{
			s_tagged_heap.shutdown();
			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		// Last, after every step that can fail. From here on a PMR container given no allocator
		// allocates from the engine heap instead of global operator new.
		std::pmr::set_default_resource(heap(Category::Unknown));
		return true;
	}

	void shutdown()
	{
		if (!s_initialized.exchange(false, std::memory_order_acq_rel))
		{
			return;
		}

		s_tagged_heap.shutdown();
		shutdown_thread();

		const u32 leaked = memory_tracker::report_leaks();
		EMBER_ASSERT_MSG(leaked == 0, "heap memory was still held at shutdown");
	}

	bool initialize_thread()
	{
		internal::heap_initialize_thread();
		return Arena::register_thread();
	}

	void shutdown_thread()
	{
		Arena::unregister_thread();
		internal::heap_shutdown_thread();
	}

	TaggedHeap& tagged_heap()
	{
		EMBER_ASSERT_MSG(s_initialized.load(std::memory_order_acquire),
		    "the tagged heap is valid between initialize and shutdown");
		return s_tagged_heap;
	}

	void out_of_memory(size_t size, size_t alignment, const char* owner)
	{
		std::fprintf(stderr, "[memory] out of memory: %s asked for %llu bytes aligned to %llu\n",
		    owner, (unsigned long long)size, (unsigned long long)alignment);
		std::fflush(stderr);

		EMBER_DEBUG_BREAK();
		std::abort();
	}
}
