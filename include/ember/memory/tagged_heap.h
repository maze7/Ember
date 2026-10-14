#pragma once

// The shared block pool behind every lifetime allocator, after the tagged heap in Christian
// Gyrling's GDC 2015 talk on parallelizing the Naughty Dog engine. One reservation is cut into
// fixed blocks. An allocator claims blocks under a tag and bump allocates inside them, and when
// the lifetime the tag names is over, every block under that tag comes back in one call. Nothing
// is freed one allocation at a time, so there is no per allocation bookkeeping, no fragmentation
// inside a lifetime and never a question of who frees what.
//
// Taking a block is a short scan under a spin lock. Allocators take a block at a time and bump
// inside it with no lock at all, so the heap is contended once per block per thread and never
// per allocation.

#include <ember/core/enum.h>
#include <ember/core/macros.h>
#include <ember/core/spin_mutex.h>
#include <ember/core/types.h>

#include <atomic>
#include <cstddef>

namespace ember
{
	// The engine's memory lifetimes: when a group of allocations dies. A tag is a lifetime plus
	// a sequence counting its turns, the frame number for a frame's memory, so one tag names one
	// turn of one lifetime and nothing reuses it until the sequence wraps.
	enum class Lifetime : u8
	{
		// The lifetime of NO_TAG. Never a real one.
		None = 0,

		// From initializatoin to shutdown.
		Permanent,

		// From a level's load to its unload.
		Level,

		// One frame, freed once nothing reads that frame any more.
		Frame,

		Count
	};

	EMBER_ENUM_NAMES(Lifetime, "None", "Permanent", "Level", "Frame");

	// What the heap frees by. The lifetime sits in the top eight bits and the sequence in the
	// other fifty-six, so checking a block's owner is one integer compare.
	using HeapTag = u64;

	inline constexpr HeapTag NO_TAG = 0;
	inline constexpr u32 HEAP_TAG_SEQUENCE_BITS = 56;
	inline constexpr u64 HEAP_TAG_SEQUENCE_MASK = (u64{1} << HEAP_TAG_SEQUENCE_BITS) - 1;

	// Builds the tag for one turn of a lifetime. The lifetime must not be None, so that no tag
	// built here is ever NO_TAG.
	constexpr HeapTag heap_tag(Lifetime lifetime, u64 sequence = 0)
	{
		EMBER_ASSERT(lifetime != Lifetime::None);
		EMBER_ASSERT(sequence <= HEAP_TAG_SEQUENCE_MASK);

		return (static_cast<HeapTag>(lifetime) << HEAP_TAG_SEQUENCE_BITS) | sequence;
	}

	constexpr Lifetime tag_lifetime(HeapTag tag)
	{
		return static_cast<Lifetime>(tag >> HEAP_TAG_SEQUENCE_BITS);
	}

	constexpr u64 tag_sequence(HeapTag tag)
	{
		return tag & HEAP_TAG_SEQUENCE_MASK;
	}

	// The bookkeeping is a free bit and an owner tag per block, kept at the front of the heap's
	// own reservation so the heap is exactly one mapping. The bits are read and written under
	// the lock. The owners are written under the lock but read anywhere, with no ordering, by
	// tag_of(): that is a diagnostic answer, not one to build a free on.
	class TaggedHeap final
	{
	public:
		TaggedHeap() = default;
		~TaggedHeap();

		TaggedHeap(const TaggedHeap&) = delete;
		TaggedHeap& operator=(const TaggedHeap&) = delete;

		// Reserves and commits the whole heap, so that taking a block is never a system call.
		// capacity rounds up to whole blocks; block_size must be a power of two. False when the
		// address space or the memory is refused.
		[[nodiscard]] bool initialize(size_t capacity, size_t block_size);

		// Returns the reservation. Every tag must have been freed first; a block still owned is a
		// leak, asserted on and released anyway.
		void shutdown();

		// Claims count consecutive blocks under tag. The lowest free run wins; null when no run
		// that long is free. Any thread may call this, but it is for an allocator refilling, not
		// for an allocation.
		[[nodiscard]] void* allocate(u32 count, HeapTag tag);

		// Returns every block owned by tag and reports how many there were. A scan over every
		// block, which with blocks this large is a few hundred loads.
		u32 free(HeapTag tag);

		// The tag owning the block ptr lies in; NO_TAG for a free block or a foreign pointer.
		HeapTag tag_of(const void* ptr) const;

		// True if ptr lies inside the heap, whoever owns that block.
		bool owns(const void* ptr) const
		{
			const u8* p = static_cast<const u8*>(ptr);
			return p >= m_base && p < m_base + m_capacity;
		}

		// Blocks a tag owns right now. A scan, for telemetry and tests.
		u32 blocks_owned(HeapTag tag) const;

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

		// One bit per block, set while free, and one owner tag per block, NO_TAG while free.
		u64* m_free = nullptr;
		std::atomic<HeapTag>* m_tags = nullptr;

		// The one mapping: bookkeeping first, then the blocks, which start at m_base.
		u8* m_reservation = nullptr;
		size_t m_reservation_size = 0;
		u8* m_base = nullptr;
		size_t m_capacity = 0;
		size_t m_block_size = 0;
		u32 m_block_count = 0;
		u32 m_word_count = 0;

		// No free bit lives below this word.
		u32 m_hint = 0;

		std::atomic<u32> m_blocks_in_use{0};
		std::atomic<u32> m_peak_blocks{0};
	};
}
