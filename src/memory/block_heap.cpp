#include <ember/core/profile.h>
#include <ember/memory/alignment.h>
#include <ember/memory/block_heap.h>
#include <ember/memory/virtual_memory.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>

namespace ember::memory
{
	namespace
	{
		constexpr u32 NO_BLOCK = ~0u;

		// free() and blocks_owned() scan every block, so this limit is well above
		// the intended pool size.
		constexpr size_t MAX_BLOCKS = size_t{1} << 24;

		// Round the tables to whole pages so the block payload starts page-aligned.
		size_t bookkeeping_size(size_t blocks)
		{
			const size_t words = (blocks + 63) / 64;
			return align_up(words * sizeof(u64) + blocks * sizeof(std::atomic<BlockTag>),
			    virtual_memory::page_size());
		}
	}

	size_t BlockHeap::region_size(size_t capacity, size_t block_size)
	{
		EMBER_ASSERT(std::has_single_bit(block_size));

		const size_t rounded = align_up(capacity, block_size);
		return bookkeeping_size(rounded / block_size) + rounded;
	}

	BlockHeap::~BlockHeap()
	{
		shutdown();
	}

	bool BlockHeap::initialize(void* region, size_t size, size_t block_size)
	{
		EMBER_ASSERT_MSG(m_base == nullptr, "initialize runs once");
		EMBER_ASSERT(region != nullptr);
		EMBER_ASSERT(std::has_single_bit(block_size));
		EMBER_ASSERT(is_aligned(region, virtual_memory::page_size()));

		if (m_base != nullptr || region == nullptr || !std::has_single_bit(block_size))
		{
			return false;
		}

		// The tables grow with the block count. Start with the whole region and give
		// back blocks until both the payload and its bookkeeping fit.
		size_t blocks = size / block_size;
		while (blocks > 0 && bookkeeping_size(blocks) + blocks * block_size > size)
		{
			--blocks;
		}

		if (blocks == 0 || blocks > MAX_BLOCKS)
		{
			return false;
		}

		const u32 words = (u32)((blocks + 63) / 64);
		const size_t bookkeeping = bookkeeping_size(blocks);
		u8* memory = (u8*)region;

		m_free = reinterpret_cast<u64*>(memory);
		m_tags = reinterpret_cast<std::atomic<BlockTag>*>(memory + words * sizeof(u64));

		for (u32 word = 0; word < words; ++word)
		{
			m_free[word] = 0;
		}

		for (u32 block = 0; block < blocks; ++block)
		{
			m_free[block / 64] |= u64{1} << (block % 64);
			std::construct_at(&m_tags[block], NO_BLOCK_TAG);
		}

		m_base = memory + bookkeeping;
		m_capacity = blocks * block_size;
		m_block_size = block_size;
		m_block_count = (u32)blocks;
		m_word_count = words;
		m_hint = 0;
		return true;
	}

	void BlockHeap::shutdown()
	{
		if (m_base == nullptr)
		{
			return;
		}

		EMBER_ASSERT_MSG(blocks_in_use() == 0,
		    "a tag still owns blocks at shutdown; every lifetime must be freed first");

		m_free = nullptr;
		m_tags = nullptr;
		m_base = nullptr;
		m_capacity = 0;
		m_block_size = 0;
		m_block_count = 0;
		m_word_count = 0;
		m_hint = 0;
		m_blocks_in_use.store(0, std::memory_order_relaxed);
		m_peak_blocks.store(0, std::memory_order_relaxed);
	}

	u32 BlockHeap::find_run(u32 count) const
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

	void* BlockHeap::allocate(u32 count, BlockTag tag)
	{
		EMBER_PROFILE_SCOPE_C("BlockAllocator::allocate", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
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

	u32 BlockHeap::free(BlockTag tag)
	{
		EMBER_PROFILE_SCOPE_C("BlockAllocator::free", PROFILE_COLOR_MEMORY);
		EMBER_ASSERT(m_base != nullptr);
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

	BlockTag BlockHeap::tag_of(const void* ptr) const
	{
		if (!owns(ptr))
		{
			return NO_BLOCK_TAG;
		}

		const size_t block = (size_t)((const u8*)ptr - m_base) / m_block_size;
		return m_tags[block].load(std::memory_order_relaxed);
	}

	u32 BlockHeap::blocks_owned(BlockTag tag) const
	{
		u32 owned = 0;
		for (u32 block = 0; block < m_block_count; ++block)
		{
			owned += m_tags[block].load(std::memory_order_relaxed) == tag;
		}

		return owned;
	}
}
