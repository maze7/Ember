#include <ember/core/profile.h>
#include <ember/memory/tagged_heap.h>

#include <algorithm>
#include <bit>
#include <cstring>

namespace ember::memory
{
	constexpr u32 NO_BLOCK = ~0u;

	TaggedHeap::TaggedHeap(u8* blocks, u32 block_count, size_t block_size)
	    : m_base(blocks), m_capacity(block_count * block_size), m_block_size(block_size),
	      m_block_count(block_count), m_word_count((block_count + 63) / 64)
	{
		EMBER_ASSERT(blocks != nullptr);
		EMBER_ASSERT(block_count != 0 && block_count <= MAX_BLOCKS);
		EMBER_ASSERT(std::has_single_bit(block_size));

		for (u32 block = 0; block < block_count; ++block)
		{
			m_free[block / 64] |= u64{1} << (block % 64);
		}
	}

	TaggedHeap::~TaggedHeap()
	{
		EMBER_ASSERT_MSG(blocks_in_use() == 0,
		    "a tag still owns blocks; every lifetime must be freed before destruction");
	}

	u32 TaggedHeap::find_run(u32 count) const
	{
		// For a single block, skip full words and take the lowest free bit.
		// m_hint avoids revisiting words that earlier scans already found full.
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

		// Larger runs can cross bitmap words, so count consecutive free blocks
		// rather than searching for a bit pattern within one word.
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

	void* TaggedHeap::allocate(u32 count, BlockTag tag)
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::allocate", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(tag != NO_BLOCK_TAG);
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

			// The lock serializes updates. Atomic counters let telemetry read them
			// without taking that lock.
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

	u32 TaggedHeap::free(BlockTag tag)
	{
		EMBER_PROFILE_SCOPE_C("TaggedHeap::free", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(tag != NO_BLOCK_TAG);

#if EMBER_MEMORY_TRACKING >= 2
		// Poison before returning blocks to the pool so these large writes stay
		// outside the spin lock. The caller has already finished this tag's users
		// and must serialize its frees, keeping the blocks from being reused during
		// this pass. The pattern helps expose accidental reads in debug builds.
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

			m_tags[block].store(NO_BLOCK_TAG, std::memory_order_relaxed);
			m_free[block / 64] |= u64{1} << (block % 64);
			lowest = std::min(lowest, block / 64);
			++freed;
		}

		// Let the next scan see the newly freed low blocks. Otherwise the hint
		// could leave usable space behind it.
		m_hint = std::min(m_hint, lowest);
		m_blocks_in_use.fetch_sub(freed, std::memory_order_relaxed);

		m_lock.unlock();
		return freed;
	}

	BlockTag TaggedHeap::tag_of(const void* ptr) const
	{
		if (!owns(ptr))
		{
			return NO_BLOCK_TAG;
		}

		const size_t block = (size_t)((const u8*)ptr - m_base) / m_block_size;
		return m_tags[block].load(std::memory_order_relaxed);
	}

	u32 TaggedHeap::blocks_owned(BlockTag tag) const
	{
		u32 owned = 0;
		for (u32 block = 0; block < m_block_count; ++block)
		{
			owned += m_tags[block].load(std::memory_order_relaxed) == tag;
		}

		return owned;
	}
}
