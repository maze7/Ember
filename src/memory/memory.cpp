#include <ember/memory/memory.h>
#include <ember/memory/virtual_memory.h>

#include "internal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace ember::memory
{
	namespace
	{
		constinit BlockHeap s_blocks;
		constinit std::atomic<bool> s_initialized{false};

		// Keep the original range for release(). Each allocator remembers the
		// partition it was given.
		constinit u8* s_reservation = nullptr;
		constinit size_t s_reservation_size = 0;
	}

	bool initialize(const MemoryDef& def)
	{
		bool expected = false;
		if (!s_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		{
			EMBER_ASSERT_MSG(false, "memory is initialized once");
			return false;
		}

		// Keep each partition page-aligned so it can be protected independently.
		// Commit the whole range now so allocators need no further commit calls.
		const size_t page = virtual_memory::page_size();
		const size_t heap = align_up((size_t)def.heap_capacity, page);
		const size_t blocks =
		    BlockHeap::region_size((size_t)def.block_capacity, (size_t)def.block_size);
		const size_t development = align_up((size_t)def.development_capacity, page);
		const size_t total =
		    align_up(heap + blocks + development, virtual_memory::allocation_granularity());

		u8* base = (u8*)virtual_memory::reserve(total);
		if (base == nullptr || !virtual_memory::commit(base, total))
		{
			std::fprintf(stderr, "[memory] the system refused a reservation of %llu bytes\n",
			    (unsigned long long)total);
			if (base != nullptr)
			{
				(void)virtual_memory::release(base, total);
			}

			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		s_reservation = base;
		s_reservation_size = total;

		u8* development_base = development != 0 ? base + heap + blocks : nullptr;
		if (!s_blocks.initialize(base + heap, blocks, (size_t)def.block_size) ||
		    !internal::heap_initialize(base, heap, development_base, development) ||
		    !initialize_thread())
		{
			std::fprintf(stderr, "[memory] the memory map could not be brought up\n");
			s_blocks.shutdown();
			internal::heap_shutdown();
			(void)virtual_memory::release(base, total);
			s_reservation = nullptr;
			s_reservation_size = 0;
			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		return true;
	}

	void shutdown()
	{
		if (!s_initialized.exchange(false, std::memory_order_acq_rel))
		{
			return;
		}

		s_blocks.shutdown();
		shutdown_thread();

		// The report needs the live allocation headers, so run it before tearing
		// down the heap.
		const u32 leaked = report_leaks();
		EMBER_ASSERT_MSG(leaked == 0, "heap memory was still held at shutdown");

		internal::heap_shutdown();
		(void)virtual_memory::release(s_reservation, s_reservation_size);
		s_reservation = nullptr;
		s_reservation_size = 0;
	}

	bool initialize_thread()
	{
		return Arena::register_thread();
	}

	void shutdown_thread()
	{
		Arena::unregister_thread();
	}

	BlockHeap* block_allocator()
	{
		EMBER_ASSERT_MSG(s_initialized.load(std::memory_order_acquire),
		    "the block allocator is valid between initialize and shutdown");
		return &s_blocks;
	}

	MemoryMap map()
	{
		MemoryMap map;
		internal::heap_map(map.heap, map.development, map.overflowed);

		const u64 block_size = s_blocks.block_size();
		map.blocks.used_bytes = (u64)s_blocks.blocks_in_use() * block_size;
		map.blocks.peak_bytes = (u64)s_blocks.peak_blocks() * block_size;
		map.blocks.capacity = s_blocks.capacity();

		map.retail_bytes = map.heap.used_bytes + map.blocks.used_bytes;
		map.retail_peak = map.heap.peak_bytes + map.blocks.peak_bytes;
		map.retail_capacity = map.heap.capacity + map.blocks.capacity;
		return map;
	}

	void out_of_memory(size_t size, size_t alignment, const char* owner)
	{
		std::fprintf(stderr, "[memory] out of memory: %s asked for %llu bytes aligned to %llu\n",
		    owner, (unsigned long long)size, (unsigned long long)alignment);
		std::fflush(stderr);

		EMBER_DEBUG_BREAK();
		std::abort();
	}

	void internal::fatal(const char* message)
	{
		std::fprintf(stderr, "[memory] %s\n", message);
		std::fflush(stderr);

		EMBER_DEBUG_BREAK();
		std::abort();
	}
}
