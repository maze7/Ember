#include <ember/core/log.h>
#include <ember/memory/arena.h>
#include <ember/memory/memory.h>

#include "internal.h"

#include <atomic>
#include <bit>
#include <cstdio>

namespace ember::memory
{
	namespace
	{
		// One bit per registered thread. Keeping registration at thread startup and
		// shutdown keeps this shared mask out of ordinary allocation.
		constinit std::atomic<u64> s_slots_in_use{0};

		// This belongs to the OS thread, even while it runs a fiber. Allocations read
		// it through thread_slot() because a resumed job may be on another worker.
		constinit thread_local u32 t_slot = Arena::NO_SLOT;

		static_assert(Arena::MAX_THREADS == 64, "the slot mask holds one bit per slot");
	}

	Arena::Arena(const char* name) : m_name(name)
	{
		EMBER_ASSERT(name != nullptr);
		internal::register_arena(this);
	}

	Arena::~Arena()
	{
		if (m_tagged_heap == nullptr)
		{
			return;
		}

		if (m_tag != NO_BLOCK_TAG)
		{
			static_cast<void>(m_tagged_heap->free(m_tag));
		}

		internal::unregister_arena(this);

		forget_blocks();
	}

	void Arena::begin(TaggedHeap& heap, BlockTag tag)
	{
		EMBER_ASSERT_MSG(!m_active, "begin on an arena that was not ended");
		EMBER_ASSERT(tag != NO_BLOCK_TAG);

		// The caller has joined the previous producers and will publish this state
		// before starting new ones. That synchronization lets us reset the ordinary
		// slot fields without making each cursor and counter atomic.
		for (Slot& slot : m_slots)
		{
			slot.clear();
		}

		m_tagged_heap = &heap;
		m_origin = reinterpret_cast<uintptr_t>(heap.base());
		m_tag = tag;
		m_active = true;
	}

	BlockTag Arena::end()
	{
		EMBER_ASSERT_MSG(m_active, "end on an arena that was not begun");

		// Discard the cursors but keep the counters for stats(). The blocks remain
		// owned by the tag until the caller returns them to the block heap.
		forget_blocks();
		m_active = false;
		return m_tag;
	}

	bool Arena::register_thread()
	{
		if (t_slot != NO_SLOT)
		{
			return true;
		}

		u64 in_use = s_slots_in_use.load(std::memory_order_relaxed);
		for (;;)
		{
			const u64 available = ~in_use;
			if (available == 0) [[unlikely]]
			{
				return false;
			}

			// Acquire pairs with the release in unregister_thread(), so a thread that inherits
			// a slot sees the previous owner's cursor writes.
			const u32 slot = static_cast<u32>(std::countr_zero(available));
			if (s_slots_in_use.compare_exchange_weak(in_use, in_use | (u64{1} << slot),
			        std::memory_order_acquire, std::memory_order_relaxed))
			{
				t_slot = slot;
				return true;
			}
		}
	}

	void Arena::unregister_thread()
	{
		if (t_slot == NO_SLOT)
		{
			return;
		}

		s_slots_in_use.fetch_and(~(u64{1} << t_slot), std::memory_order_release);
		t_slot = NO_SLOT;
	}

	u32 Arena::thread_slot()
	{
		return t_slot;
	}

	size_t Arena::blocks_needed(size_t bytes, size_t alignment) const
	{
		// A run starts on a block boundary, which is already aligned to anything up to the
		// block size. Larger alignments reserve worst-case padding.
		const size_t block_size = m_tagged_heap->block_size();
		const size_t slack = alignment <= block_size ? 0 : alignment - 1;
		return (bytes + slack + block_size - 1) / block_size;
	}

	u8* Arena::refill(Slot& slot, size_t bytes, size_t alignment)
	{
		const size_t count = blocks_needed(bytes, alignment);
		u8* base = count <= m_tagged_heap->block_count()
		               ? (u8*)m_tagged_heap->allocate((u32)count, m_tag)
		               : nullptr;

		if (base == nullptr) [[unlikely]]
		{
			return nullptr;
		}

		u8* aligned = align_offset(base, alignment);
		EMBER_ASSERT(aligned == base || alignment > m_tagged_heap->block_size());

		// Leave the old run's unused tail for tag reclamation. Each thread can then
		// keep allocating through a single forward-only cursor.
		slot.cursor = aligned + bytes;
		slot.end = base + count * m_tagged_heap->block_size();
		slot.refills += 1;
		slot.blocks += (u32)count;
		return aligned;
	}

	void Arena::exhausted(size_t size, size_t alignment) const
	{
		EMBER_ERROR("[memory] arena '{}': no run of {} free blocks; {} of {} in use.", m_name,
		    static_cast<u64>(blocks_needed(size == 0 ? 1 : size, alignment)),
		    m_tagged_heap->blocks_in_use(), m_tagged_heap->block_count());
		out_of_memory(size, alignment, m_name);
	}

	void Arena::forget_blocks()
	{
		for (Slot& slot : m_slots)
		{
			slot.cursor = nullptr;
			slot.end = nullptr;
		}
	}

	Arena::Stats Arena::stats() const
	{
		Stats stats;

		for (const Slot& slot : m_slots)
		{
			if (slot.refills == 0)
			{
				continue;
			}

			stats.bytes_requested += slot.bytes;
			stats.blocks += slot.blocks;
			stats.refills += slot.refills;
			stats.threads += 1;
		}

		stats.bytes_owned =
		    m_tagged_heap != nullptr ? stats.blocks * m_tagged_heap->block_size() : 0;
		return stats;
	}

	u64 Arena::tag_bytes(MemoryTag tag) const
	{
#if EMBER_MEMORY_TRACKING >= 1
		u64 bytes = 0;
		for (const Slot& slot : m_slots)
		{
			bytes += slot.bytes_by_tag[(size_t)tag].load(std::memory_order_relaxed);
		}

		return bytes;
#else
		(void)tag;
		return 0;
#endif
	}

	void Arena::deallocate(void* ptr, size_t, size_t)
	{
		// Individual frees do not reclaim arena storage. Checking the current tag
		// helps catch foreign pointers and containers used after a lifetime change.
		EMBER_ASSERT_MSG(ptr == nullptr || owns(ptr),
		    "freeing memory this arena did not hand out under its current tag");
	}
}
