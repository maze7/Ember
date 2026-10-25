#pragma once

// Allocations share a lifetime. Each thread advances a cursor through its own
// blocks; only a refill visits the shared block heap.
//
// Individual allocations stay put until their tag is freed. end() leaves the
// blocks in place so readers can finish before the caller returns them:
//
//     scratch.begin(block_tag(Lifetime::Frame, N));
//     ... update and render allocate from registered workers ...
//     const BlockTag done = scratch.end();
//     blocks->free(done); // Once every CPU and GPU reader has finished.
//
// Fibers may resume on another worker, so each allocation looks up the current
// thread's slot through an out-of-line call. Allocation does not suspend the
// fiber, which lets it finish using that cursor without a lock. There are no
// rewind marks: a mark saved across a fiber wait could belong to the old worker.
//
// Reclaiming arena storage does not run object destructors. Arrange any required
// cleanup before returning the object's blocks.
//
// BlockTag names the lifetime; memory::Tag names the subsystem using the bytes.
// Tracking builds keep per-thread totals for memory::stats(). Shipping builds
// leave out those counters.

#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>
#include <ember/memory/block_heap.h>

#include <atomic>
#include <cstddef>
#include <source_location>

namespace ember::memory
{
	class Arena final : public Allocator
	{
	public:
		// Each registered thread needs one bit in the shared slot mask.
		static constexpr u32 MAX_THREADS = 64;
		static constexpr u32 NO_SLOT = ~0u;

		// Allocation totals since begin(). Read these after the producers have
		// finished; the ordinary per-thread counters cannot be read concurrently.
		struct Stats
		{
			// Bytes handed out, counting zero-byte requests as one byte, and the total
			// size of the blocks acquired. The gap is padding and unused block space.
			size_t bytes_requested = 0;
			size_t bytes_owned = 0;

			u32 blocks = 0;

			// Refills include oversized runs. threads counts slots used this lifetime.
			u32 refills = 0;
			u32 threads = 0;
		};

		Arena() = default;
		~Arena() override;

		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		// The block heap and diagnostic name must outlive the arena's use of them.
		// Call begin() before allocating.
		void initialize(BlockHeap* blocks, const char* name);

		// Returns blocks still owned by the current tag, then detaches from the heap.
		// All users must have finished first. Earlier tags remain the caller's
		// responsibility, even if this arena has already started another lifetime.
		void shutdown();

		// Starts allocating under tag. Finish the previous producers before resetting
		// the slots, and publish this state before starting new ones.
		// Blocks belonging to earlier tags remain available to their readers.
		void begin(BlockTag tag);

		// Ends allocation after all producers have finished and returns the tag.
		// The caller frees it once the remaining readers have finished too.
		BlockTag end();

		// Register before using any arena and release the slot when the thread stops
		// using arenas. Change registrations only while arena users are quiescent.
		// Registering twice keeps the same slot. Returns false when all slots are taken.
		[[nodiscard]] static bool register_thread();
		static void unregister_thread();

		// Returns the calling thread's slot, or NO_SLOT if it is unregistered.
		// Keep this out of line: a fiber may resume on another worker, so allocations
		// must look up that worker's slot rather than keep an earlier TLS lookup.
		static EMBER_NOINLINE u32 thread_slot();

		// Requires an active lifetime. A zero-byte request consumes one byte so it
		// still gets a distinct live address. Shipping builds omit tag accounting.
		[[nodiscard]] EMBER_FINLINE void* allocate(Tag tag, size_t size,
		    size_t alignment = DEFAULT_ALIGNMENT)
		{
			EMBER_ASSERT_MSG(m_active, "allocating from an arena between end() and begin()");
			EMBER_ASSERT(std::has_single_bit(alignment));

			Slot& slot = current_slot();
			const size_t bytes = size == 0 ? 1 : size;
			u8* aligned = align_up(slot.cursor, alignment);

			slot.bytes += bytes;
			charge(slot, tag, bytes);

			// Check the aligned cursor before subtracting: alignment can carry it past
			// the block. An empty slot also needs a refill.
			if (aligned >= slot.end || (size_t)(slot.end - aligned) < bytes) [[unlikely]]
			{
				return refill(slot, bytes, alignment);
			}

			slot.cursor = aligned + bytes;
			return aligned;
		}

		// Arena tracking records subsystem totals, not individual allocation sites.
		void* allocate(Tag tag, size_t size, size_t alignment, std::source_location) override
		{
			return allocate(tag, size, alignment);
		}

		void deallocate(void* ptr, size_t, size_t) override;

		// Checks whether ptr lies in a block owned by the arena's current tag.
		// Switching tags changes this result even if the older blocks are still live.
		// Block ownership alone cannot detect stale pointers after address reuse.
		bool owns(const void* ptr) const
		{
			return m_blocks != nullptr && m_tag != NO_BLOCK_TAG && m_blocks->tag_of(ptr) == m_tag;
		}

		bool is_active() const
		{
			return m_active;
		}

		BlockTag tag() const
		{
			return m_tag;
		}

		const char* name() const
		{
			return m_name;
		}

		Stats stats() const;

		// Subsystem bytes allocated since begin(). Safe to sample during allocation,
		// but the slots are read separately, so the total is not a synchronized
		// snapshot. Counts reset at the next begin(). Shipping builds return zero.
		u64 tag_bytes(Tag tag) const;

	private:
		// Keep each thread's cursor on separate cache lines to avoid false sharing.
		// Tracking counters follow it and are atomic so telemetry can read them during
		// allocation. Each counter has one writer, so charging it needs no atomic
		// read-modify-write operation.
		struct alignas(EMBER_CACHE_LINE) Slot
		{
			u8* cursor = nullptr;
			u8* end = nullptr;
			size_t bytes = 0;
			u32 refills = 0;
			u32 blocks = 0;

#if EMBER_MEMORY_TRACKING >= 1
			std::atomic<u64> bytes_by_tag[TAG_COUNT];
#endif

			void clear()
			{
				cursor = nullptr;
				end = nullptr;
				bytes = 0;
				refills = 0;
				blocks = 0;

#if EMBER_MEMORY_TRACKING >= 1
				for (std::atomic<u64>& held : bytes_by_tag)
				{
					held.store(0, std::memory_order_relaxed);
				}
#endif
			}
		};

		EMBER_FINLINE static void charge(Slot& slot, Tag tag, size_t bytes)
		{
#if EMBER_MEMORY_TRACKING >= 1
			std::atomic<u64>& held = slot.bytes_by_tag[(size_t)tag];
			held.store(held.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
#else
			(void)slot;
			(void)tag;
			(void)bytes;
#endif
		}

		EMBER_FINLINE Slot& current_slot()
		{
			const u32 slot = thread_slot();
			EMBER_ASSERT_MSG(slot < MAX_THREADS, "arena memory belongs to registered threads");
			return m_slots[slot];
		}

		void* refill(Slot& slot, size_t bytes, size_t alignment);
		void forget_blocks();

		Slot m_slots[MAX_THREADS];
		BlockHeap* m_blocks = nullptr;
		BlockTag m_tag = NO_BLOCK_TAG;
		const char* m_name = "arena";
		bool m_active = false;
	};
}
