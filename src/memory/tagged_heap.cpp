#include <ember/core/profile.h>
#include <ember/memory/alignment.h>
#include <ember/memory/tagged_heap.h>
#include <ember/memory/virtual_memory.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>

namespace ember
{
	namespace
	{
		constexpr u32 NO_BLOCK = ~0u;

		// Sixteen million blocks. The scans in free() and blocks_owned() are built for hundreds.
		constexpr size_t MAX_BLOCKS = size_t{1} << 24;
	}

	TaggedHeap::~TaggedHeap()
	{
		shutdown();
	}

	bool TaggedHeap::initialize(size_t capacity, size_t block_size)
	{
		EMBER_ASSERT_MSG(m_base == nullptr, "initialize runs once");
		EMBER_ASSERT(std::has_single_bit(block_size));
		EMBER_ASSERT(capacity >= block_size);

		if (m_base != nullptr || !std::has_single_bit(block_size) || capacity < block_size)
		{
			return false;
		}

		const size_t rounded = align_up(capacity, block_size);
		const size_t blocks = rounded / block_size;
		if (blocks > MAX_BLOCKS)
		{
			return false;
		}

		// The bookkeeping takes the front of the reservation, rounded out to a page so the first
		// block starts on one. Both parts are committed now: a block is then a few loads under a
		// short lock and never a system call, while physical pages still arrive on first touch.
		const u32 words = (u32)((blocks + 63) / 64);
		const size_t bookkeeping =
		    align_up(words * sizeof(u64) + blocks * sizeof(std::atomic<HeapTag>),
		        virtual_memory::page_size());
		const size_t reservation =
		    align_up(bookkeeping + rounded, virtual_memory::allocation_granularity());

		u8* memory = (u8*)virtual_memory::reserve(reservation);
		if (memory == nullptr)
		{
			return false;
		}

		if (!virtual_memory::commit(memory, reservation))
		{
			(void)virtual_memory::release(memory, reservation);
			return false;
		}

		m_free = reinterpret_cast<u64*>(memory);
		m_tags = reinterpret_cast<std::atomic<HeapTag>*>(memory + words * sizeof(u64));

		for (u32 word = 0; word < words; ++word)
		{
			m_free[word] = 0;
		}

		for (u32 block = 0; block < blocks; ++block)
		{
			m_free[block / 64] |= u64{1} << (block % 64);
			std::construct_at(&m_tags[block], NO_TAG);
		}

		m_reservation = memory;
		m_reservation_size = reservation;
		m_base = memory + bookkeeping;
		m_capacity = rounded;
		m_block_size = block_size;
		m_block_count = (u32)blocks;
		m_word_count = words;
		m_hint = 0;
		return true;
	}

	void TaggedHeap::shutdown()
	{
		if (m_base == nullptr)
		{
			return;
		}

		EMBER_ASSERT_MSG(blocks_in_use() == 0,
		    "a tag still owns blocks at shutdown; every lifetime must be freed first");

		(void)virtual_memory::release(m_reservation, m_reservation_size);

		m_free = nullptr;
		m_tags = nullptr;
		m_reservation = nullptr;
		m_reservation_size = 0;
		m_base = nullptr;
		m_capacity = 0;
		m_block_size = 0;
		m_block_count = 0;
		m_word_count = 0;
		m_hint = 0;
		m_blocks_in_use.store(0, std::memory_order_relaxed);
		m_peak_blocks.store(0, std::memory_order_relaxed);
	}

	u32 TaggedHeap::find_run(u32 count) const
	{
		// Nearly every request is for one block: the first set bit of the first word with any,
		// starting where the last scan proved everything below to be full.
		if (count == 1)
		{
			for (u32 word = m_hint; word < m_word_count; ++word)
			{
				if (m_free[word] != 0)
				{
					return word * 64 + (u32)std::countr_zero(m_free[word]);
				}
			}

			return NO_BLOCK;
		}

		// A run has to be consecutive, so walk the bits and count. Bits past block_count are never
		// set, which bounds the walk without a second limit.
		u32 run = 0;
		for (u32 block = m_hint * 64; block < m_block_count; ++block)
		{
			if (!is_free(block))
			{
				run = 0;
				continue;
			}

			if (++run == count)
			{
				return block + 1 - count;
			}
		}

		return NO_BLOCK;
	}

	void* TaggedHeap::allocate(u32 count, HeapTag tag)
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::allocate", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
		EMBER_ASSERT(tag != NO_TAG);
		EMBER_ASSERT(count != 0);

		if (count == 0 || count > m_block_count)
		{
			return nullptr;
		}

		m_lock.lock();

		const u32 first = find_run(count);
		if (first != NO_BLOCK)
		{
			for (u32 block = first; block < first + count; ++block)
			{
				m_free[block / 64] &= ~(u64{1} << (block % 64));
				m_tags[block].store(tag, std::memory_order_relaxed);
			}

			if (count == 1)
			{
				m_hint = first / 64;
			}

			// Both counters change only under the lock; they are atomic for the readers outside it.
			const u32 in_use = m_blocks_in_use.load(std::memory_order_relaxed) + count;
			m_blocks_in_use.store(in_use, std::memory_order_relaxed);
			if (in_use > m_peak_blocks.load(std::memory_order_relaxed))
			{
				m_peak_blocks.store(in_use, std::memory_order_relaxed);
			}
		}

		m_lock.unlock();

		return first == NO_BLOCK ? nullptr : m_base + (size_t)first * m_block_size;
	}

	u32 TaggedHeap::free(HeapTag tag)
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::free", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
		EMBER_ASSERT(tag != NO_TAG);

#if EMBER_MEMORY_TRACKING >= 2
		// Before the lock: the blocks are still this tag's, so nobody else can take them, and a
		// memset the size of a block has no business under a spin lock. A stale pointer into the
		// lifetime then reads the pattern instead of last frame's data.
		for (u32 block = 0; block < m_block_count; ++block)
		{
			if (m_tags[block].load(std::memory_order_relaxed) == tag)
			{
				std::memset(m_base + (size_t)block * m_block_size, 0xDC, m_block_size);
			}
		}
#endif

		u32 freed = 0;
		u32 lowest = m_word_count;

		m_lock.lock();

		for (u32 block = 0; block < m_block_count; ++block)
		{
			if (m_tags[block].load(std::memory_order_relaxed) != tag)
			{
				continue;
			}

			m_tags[block].store(NO_TAG, std::memory_order_relaxed);
			m_free[block / 64] |= u64{1} << (block % 64);
			lowest = std::min(lowest, block / 64);
			++freed;
		}

		// Pull the scan back to the lowest word this tag returned, so the heap refills from the
		// bottom instead of drifting upwards, and free runs stay contiguous for longer.
		m_hint = std::min(m_hint, lowest);
		m_blocks_in_use.fetch_sub(freed, std::memory_order_relaxed);

		m_lock.unlock();
		return freed;
	}

	HeapTag TaggedHeap::tag_of(const void* ptr) const
	{
		if (!owns(ptr))
		{
			return NO_TAG;
		}

		const size_t block = (size_t)((const u8*)ptr - m_base) / m_block_size;
		return m_tags[block].load(std::memory_order_relaxed);
	}

	u32 TaggedHeap::blocks_owned(HeapTag tag) const
	{
		u32 owned = 0;
		for (u32 block = 0; block < m_block_count; ++block)
		{
			owned += m_tags[block].load(std::memory_order_relaxed) == tag;
		}

		return owned;
	}
}
