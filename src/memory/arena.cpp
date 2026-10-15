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

	Arena::~Arena()
	{
		shutdown();
	}

	void Arena::initialize(BlockHeap* blocks, const char* name)
	{
		EMBER_ASSERT_MSG(m_blocks == nullptr, "initialize runs once");
		EMBER_ASSERT(blocks != nullptr);
		EMBER_ASSERT(name != nullptr);

		m_blocks = blocks;
		m_name = name;
		m_tag = NO_BLOCK_TAG;
		m_active = false;

		internal::register_arena(this);
	}

	void Arena::shutdown()
	{
		if (m_blocks == nullptr)
		{
			return;
		}

		if (m_tag != NO_BLOCK_TAG)
		{
			(void)m_blocks->free(m_tag);
		}

		internal::unregister_arena(this);

		forget_blocks();
		m_blocks = nullptr;
		m_tag = NO_BLOCK_TAG;
		m_active = false;
	}

	void Arena::begin(BlockTag tag)
	{
		EMBER_ASSERT_MSG(m_blocks != nullptr, "begin before initialize");
		EMBER_ASSERT_MSG(!m_active, "begin on an arena that was not ended");
		EMBER_ASSERT(tag != NO_BLOCK_TAG);

		// The caller has joined the previous producers and will publish this state
		// before starting new ones. That synchronization lets us reset the ordinary
		// slot fields without making each cursor and counter atomic.
		for (Slot& slot : m_slots)
		{
			slot.clear();
		}

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

			const u32 slot = (u32)std::countr_zero(available);
			if (s_slots_in_use.compare_exchange_weak(in_use, in_use | (u64{1} << slot),
			        std::memory_order_relaxed))
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

		s_slots_in_use.fetch_and(~(u64{1} << t_slot), std::memory_order_relaxed);
		t_slot = NO_SLOT;
	}

	u32 Arena::thread_slot()
	{
		return t_slot;
	}

	void* Arena::refill(Slot& slot, size_t bytes, size_t alignment)
	{
		EMBER_ASSERT(m_blocks != nullptr);

		// Reserve enough space to satisfy the alignment even at the start of a run.
		const size_t block_size = m_blocks->block_size();
		const size_t needed = bytes + alignment - 1;
		const size_t count = (needed + block_size - 1) / block_size;

		u8* base =
		    count <= m_blocks->block_count() ? (u8*)m_blocks->allocate((u32)count, m_tag) : nullptr;

		if (base == nullptr) [[unlikely]]
		{
			std::fprintf(stderr,
			    "[memory] arena '%s': no run of %llu free blocks; %u of %u in use\n", m_name,
			    (unsigned long long)count, m_blocks->blocks_in_use(), m_blocks->block_count());
			out_of_memory(bytes, alignment, m_name);
		}

		u8* aligned = align_up(base, alignment);

		// Leave the old run's unused tail for tag reclamation. Each thread can then
		// keep allocating through a single forward-only cursor.
		slot.cursor = aligned + bytes;
		slot.end = base + count * block_size;
		slot.refills += 1;
		slot.blocks += (u32)count;
		return aligned;
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

		stats.bytes_owned = m_blocks != nullptr ? stats.blocks * m_blocks->block_size() : 0;
		return stats;
	}

	u64 Arena::tag_bytes(Tag tag) const
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
