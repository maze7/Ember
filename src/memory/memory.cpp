#include <ember/memory/memory.h>
#include <ember/memory/virtual_memory.h>

#include "internal.h"

#include <atomic>
#include <cstdio>

namespace ember::memory
{
	namespace
	{
		constinit TaggedHeap* s_blocks = nullptr;
		constinit std::atomic<bool> s_initialized{false};

		// Keep the original range for release(). Each allocator remembers the
		// partition it was given.
		constinit u8* s_reservation = nullptr;
		constinit size_t s_reservation_size = 0;

		// The fiber stack partition, handed out from the bottom by allocate_stacks()
		// and taken back most recent first.
		constinit u8* s_stacks = nullptr;
		constinit size_t s_stack_capacity = 0;
		constinit size_t s_stacks_used = 0;
		constinit size_t s_stacks_peak = 0;
	}

	bool initialize(const MemoryDef& def)
	{
		bool expected = false;
		if (!s_initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
		{
			EMBER_ASSERT_MSG(false, "memory is initialized once");
			return false;
		}

		EMBER_ASSERT_MSG(std::has_single_bit(def.block_size), "block_size must be a power of two");

		// Keep each partition page-aligned so it can be protected independently.
		// Commit the whole range now so allocators need no further commit calls.
		// The block partition is whole blocks, rounded to a page for small blocks.
		const size_t page = virtual_memory::page_size();
		const u64 block_count = align_up(def.block_capacity, def.block_size) / def.block_size;
		const size_t heap_size = align_up(def.heap_capacity, page);
		const size_t blocks_size = align_up(block_count * def.block_size, page);
		const size_t stacks_size = align_up(def.stack_capacity, page);
		const size_t development_size = align_up(def.development_capacity, page);
		const size_t total = align_up(heap_size + blocks_size + stacks_size + development_size,
		    virtual_memory::allocation_granularity());

		EMBER_ASSERT_MSG(block_count != 0 && block_count <= TaggedHeap::MAX_BLOCKS,
		    "block_capacity must hold between one block and TaggedHeap::MAX_BLOCKS");

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
		s_stack_capacity = stacks_size;

		u8* blocks_base = base + heap_size;
		u8* stacks_base = blocks_base + blocks_size;
		u8* development_base = development_size != 0 ? stacks_base + stacks_size : nullptr;

		if (!internal::heap_initialize(base, heap_size, development_base, development_size) ||
		    !initialize_thread())
		{
			std::fprintf(stderr, "[memory] the memory map could not be brought up\n");
			internal::heap_shutdown();
			(void)virtual_memory::release(base, total);
			s_reservation = nullptr;
			s_reservation_size = 0;
			s_initialized.store(false, std::memory_order_release);
			return false;
		}

		s_blocks = new_object<TaggedHeap>(heap(), MemoryTag::Engine, blocks_base, block_count,
		    def.block_size);
		s_stacks = stacks_size != 0 ? stacks_base : nullptr;
		s_stacks_used = 0;
		s_stacks_peak = 0;

		return true;
	}

	void shutdown()
	{
		if (!s_initialized.exchange(false, std::memory_order_acq_rel))
		{
			return;
		}

		delete_object(heap(), s_blocks);
		s_blocks = nullptr;
		shutdown_thread();

		// The report needs the live allocation headers, so run it before tearing
		// down the heap.
		const u32 leaked = report_leaks();
		EMBER_ASSERT_MSG(leaked == 0, "heap memory was still held at shutdown");

		EMBER_ASSERT_MSG(s_stacks_used == 0, "fiber stacks were still held at shutdown");
		s_stacks = nullptr;
		s_stack_capacity = 0;
		s_stacks_used = 0;
		s_stacks_peak = 0;

		internal::heap_shutdown();
		(void)virtual_memory::release(s_reservation, s_reservation_size);
		s_reservation = nullptr;
		s_reservation_size = 0;
	}

	u8* allocate_stacks(size_t size)
	{
		EMBER_ASSERT_MSG(s_initialized.load(std::memory_order_acquire),
		    "fiber stacks are valid between initialize and shutdown");

		const size_t rounded = align_up(size, virtual_memory::page_size());
		if (size == 0 || rounded > s_stack_capacity - s_stacks_used)
		{
			return nullptr;
		}

		u8* stacks = s_stacks + s_stacks_used;
		s_stacks_used += rounded;
		if (s_stacks_used > s_stacks_peak)
		{
			s_stacks_peak = s_stacks_used;
		}

		return stacks;
	}

	void free_stacks(u8* stacks, size_t size)
	{
		const size_t rounded = align_up(size, virtual_memory::page_size());
		EMBER_ASSERT_MSG(stacks != nullptr && stacks + rounded == s_stacks + s_stacks_used,
		    "fiber stacks are returned most recent first");

		// The caller decommitted a guard page below each stack. Commit the range
		// again so the next caller lays out its own guards over committed memory.
		if (!virtual_memory::commit(stacks, rounded))
		{
			out_of_memory(rounded, virtual_memory::page_size(), "fiber stacks");
		}

		s_stacks_used -= rounded;
	}

	bool initialize_thread()
	{
		return Arena::register_thread();
	}

	void shutdown_thread()
	{
		Arena::unregister_thread();
	}

	TaggedHeap& tagged_heap()
	{
		EMBER_ASSERT_MSG(s_initialized.load(std::memory_order_acquire),
		    "the block allocator is valid between initialize and shutdown");
		return *s_blocks;
	}

	MemoryMap map()
	{
		MemoryMap map;
		internal::heap_map(map.heap, map.development, map.overflowed);

		if (s_blocks != nullptr)
		{
			const u64 block_size = s_blocks->block_size();
			map.blocks.used_bytes = s_blocks->blocks_in_use() * block_size;
			map.blocks.peak_bytes = s_blocks->peak_blocks() * block_size;
			map.blocks.capacity = s_blocks->capacity();
		}

		map.stacks.used_bytes = s_stacks_used;
		map.stacks.peak_bytes = s_stacks_peak;
		map.stacks.capacity = s_stack_capacity;

		map.retail_bytes = map.heap.used_bytes + map.blocks.used_bytes + map.stacks.used_bytes;
		map.retail_peak = map.heap.peak_bytes + map.blocks.peak_bytes + map.stacks.peak_bytes;
		map.retail_capacity = map.heap.capacity + map.blocks.capacity + map.stacks.capacity;

		return map;
	}

	void out_of_memory(size_t size, size_t alignment, const char* owner)
	{
		std::fprintf(stderr, "[memory] out of memory: %s asked for %llu bytes aligned to %llu\n",
		    owner, (unsigned long long)size, (unsigned long long)alignment);
		std::fflush(stderr);

		EMBER_DEBUG_BREAK();
		halt();
	}

	void internal::fatal(const char* message)
	{
		std::fprintf(stderr, "[memory] %s\n", message);
		std::fflush(stderr);

		EMBER_DEBUG_BREAK();
		halt();
	}
}
