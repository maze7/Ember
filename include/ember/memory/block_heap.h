#pragma once

// Fixed-size blocks reclaimed in groups, following the tagged heap in Christian
// Gyrling's GDC 2015 talk about parallelizing the Naughty Dog engine.
//
// Allocators claim blocks under a tag and divide them into smaller allocations.
// Once the lifetime ends, free() returns every block carrying its tag.
//
// Claiming blocks holds a spin lock. Arena allocations use that lock only when
// refilling their private blocks.

#include <ember/core/enum.h>
#include <ember/core/macros.h>
#include <ember/core/spin_mutex.h>
#include <ember/core/types.h>

#include <atomic>
#include <cstddef>

namespace ember::memory
{
	// Lifetimes describe when a group of allocations can be reclaimed.
	// The sequence distinguishes overlapping instances, usually by frame number.
	// Callers must choose distinct tags while their blocks remain live.
	enum class Lifetime : u8
	{
		// Reserved for NO_BLOCK_TAG.
		None = 0,

		// From initialization until shutdown.
		Permanent,

		// From loading a level until unloading it.
		Level,

		// Until the last reader of a frame has finished.
		Frame,

		Count
	};

	EMBER_ENUM_NAMES(Lifetime, "None", "Permanent", "Level", "Frame");

	// Identifies a group of blocks to reclaim together. The upper eight bits hold
	// the lifetime and the remaining 56 hold its sequence. memory::Tag instead
	// identifies the subsystem using an allocation.
	using BlockTag = u64;

	inline constexpr BlockTag NO_BLOCK_TAG = 0;
	inline constexpr u32 BLOCK_TAG_SEQUENCE_BITS = 56;
	inline constexpr u64 BLOCK_TAG_SEQUENCE_MASK = (u64(1) << BLOCK_TAG_SEQUENCE_BITS) - 1;

	// None is reserved for the empty tag, and the sequence must fit in 56 bits.
	constexpr BlockTag block_tag(Lifetime lifetime, u64 sequence = 0)
	{
		EMBER_ASSERT(lifetime != Lifetime::None);
		EMBER_ASSERT(sequence <= BLOCK_TAG_SEQUENCE_MASK);

		return (static_cast<BlockTag>(lifetime) << BLOCK_TAG_SEQUENCE_BITS) | sequence;
	}

	constexpr Lifetime block_tag_lifetime(BlockTag tag)
	{
		return static_cast<Lifetime>(tag >> BLOCK_TAG_SEQUENCE_BITS);
	}

	constexpr u64 block_tag_sequence(BlockTag tag)
	{
		return tag & BLOCK_TAG_SEQUENCE_MASK;
	}

	// The free bits and owner tags live at the front of the supplied region.
	// The lock protects free bits and ownership changes. Atomic owner tags let
	// diagnostics inspect ownership without that lock. Reading block contents
	// still requires the caller's lifetime synchronization.
	class BlockHeap final
	{
	public:
		BlockHeap() = default;
		~BlockHeap();

		BlockHeap(const BlockHeap&) = delete;
		BlockHeap& operator=(const BlockHeap&) = delete;

		// Storage needed for capacity bytes of blocks and their bookkeeping.
		// The payload rounds up to whole blocks and the bookkeeping to whole pages.
		static size_t region_size(size_t capacity, size_t block_size);

		// Uses an already committed, page-aligned region for tables and blocks.
		// Returns false if the region or block size cannot form a valid pool.
		[[nodiscard]] bool initialize(void* region, size_t size, size_t block_size);

		// Drops the views into storage owned by the reservation. All clients must
		// have stopped and every tag must have been freed first.
		// Debug builds check for blocks that are still owned.
		void shutdown();

		// Claims the lowest available run of count consecutive blocks under tag.
		// Holds the pool lock. Returns null if no suitable run remains.
		[[nodiscard]] void* allocate(u32 count, BlockTag tag);

		// Returns every block carrying tag and reports how many were reclaimed.
		// Stop allocating under this tag and finish all its readers before calling.
		// Calls that free the same tag must not overlap.
		u32 free(BlockTag tag);

		// Returns the current owner of the block containing ptr, or NO_BLOCK_TAG for
		// a free block or a pointer outside the payload.
		BlockTag tag_of(const void* ptr) const;

		// Tests whether ptr lies in the block payload, including currently free blocks.
		bool owns(const void* ptr) const
		{
			const u8* p = reinterpret_cast<const u8*>(ptr);
			return p >= m_base && p < m_base + m_capacity;
		}

		// Samples the owner table for telemetry and tests. Concurrent ownership
		// changes mean the result is not a synchronized snapshot.
		u32 blocks_owned(BlockTag tag) const;

		size_t block_size() const
		{
			return m_block_size;
		}

		size_t capacity() const
		{
			return m_capacity;
		}

		u32 block_count() const
		{
			return m_block_count;
		}

		u32 blocks_in_use() const
		{
			return m_blocks_in_use.load(std::memory_order_relaxed);
		}

		u32 peak_blocks() const
		{
			return m_peak_blocks.load(std::memory_order_relaxed);
		}

	private:
		u32 find_run(u32 count) const;

		bool is_free(u32 block) const
		{
			return ((m_free[block / 64] >> (block % 64)) & 1) != 0;
		}

		SpinMutex m_lock;

		// A set bit means free. A free block's owner is NO_BLOCK_TAG.
		u64* m_free = nullptr;
		std::atomic<BlockTag>* m_tags = nullptr;

		// m_base starts after the bookkeeping and points to the block payload.
		u8* m_base = nullptr;
		size_t m_capacity = 0;
		size_t m_block_size = 0;
		u32 m_block_count = 0;
		u32 m_word_count = 0;

		// All words before this one are known to contain no free blocks.
		u32 m_hint = 0;

		std::atomic<u32> m_blocks_in_use{0};
		std::atomic<u32> m_peak_blocks{0};
	};
}
