#include <ember/memory/arena.h>
#include <ember/memory/memory.h>

#include <atomic>
#include <bit>
#include <cstdio>

namespace ember
{
	namespace
	{
		// One bit per slot, set while a thread holds it. Threads come and go at startup and
		// shutdown, so the mask is only ever contended there.
		constinit std::atomic<u64> s_slots_in_use{0};

		// The one thread local. thread_slot() is its only reader and lives in this file, out of
		// line, so no caller can keep its address across a fiber switch.
		constinit thread_local u32 t_slot = Arena::NO_SLOT;

		static_assert(Arena::MAX_THREADS == 64, "the slot mask holds one bit per slot");
	}

	Arena::~Arena()
	{
		shutdown();
	}

	void Arena::initialize(TaggedHeap& heap, const char* name)
	{
		EMBER_ASSERT_MSG(m_heap == nullptr, "initialize runs once");
		EMBER_ASSERT(name != nullptr);

		m_heap = &heap;
		m_name = name;
		m_tag = NO_TAG;
		m_active = false;
	}

	void Arena::shutdown()
	{
		if (m_heap == nullptr)
		{
			return;
		}

		if (m_tag != NO_TAG)
		{
			(void)m_heap->free(m_tag);
		}

		forget_blocks();
		m_heap = nullptr;
		m_tag = NO_TAG;
		m_active = false;
	}

	void Arena::begin(HeapTag tag)
	{
		EMBER_ASSERT_MSG(m_heap != nullptr, "begin before initialize");
		EMBER_ASSERT_MSG(!m_active, "begin on an arena that was not ended");
		EMBER_ASSERT(tag != NO_TAG);

		// Plain stores throughout. The caller owns the lifetime and sits between the join that
		// proved the last producers finished and the kick that starts the next ones, and those
		// two publish.
		for (Slot& slot : m_slots)
		{
			slot = {};
		}

		m_tag = tag;
		m_active = true;
	}

	HeapTag Arena::end()
	{
		EMBER_ASSERT_MSG(m_active, "end on an arena that was not begun");

		// The cursors go and the counters stay for stats(). The blocks stay the tag's until the
		// heap is told otherwise.
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
		EMBER_ASSERT(m_heap != nullptr);

		// Room for the request plus whatever aligning inside the first block costs.
		const size_t block_size = m_heap->block_size();
		const size_t needed = bytes + alignment - 1;
		const size_t count = (needed + block_size - 1) / block_size;

		u8* base =
		    count <= m_heap->block_count() ? (u8*)m_heap->allocate((u32)count, m_tag) : nullptr;

		if (base == nullptr) [[unlikely]]
		{
			std::fprintf(stderr,
			    "[memory] arena '%s': no run of %llu free blocks; %u of %u in use\n", m_name,
			    (unsigned long long)count, m_heap->blocks_in_use(), m_heap->block_count());
			memory::out_of_memory(bytes, alignment, m_name);
		}

		u8* aligned = align_up(base, alignment);

		// Whatever was left in the previous block is lost. A thread abandons the tail of one
		// block per refill, which is the price of handing out memory without a lock.
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

		stats.bytes_owned = m_heap != nullptr ? stats.blocks * m_heap->block_size() : 0;
		return stats;
	}

	void* Arena::do_allocate(size_t bytes, size_t alignment)
	{
		return allocate(bytes, alignment);
	}

	void Arena::do_deallocate(void* ptr, size_t, size_t)
	{
		// Reclaimed wholesale when the tag is freed. The check is the one thing worth doing: a
		// pointer the current tag does not own came from a lifetime that has already ended.
		EMBER_ASSERT_MSG(ptr == nullptr || owns(ptr),
		    "freeing memory this arena did not hand out under its current tag");
	}

	bool Arena::do_is_equal(const Allocator& other) const noexcept
	{
		return this == &other;
	}
}
