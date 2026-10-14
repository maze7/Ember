#pragma once

// The allocator for one lifetime: a bump allocator over tagged heap blocks. Every thread that
// allocates holds its own block and bumps a cursor through it, so an allocation is a few
// instructions with no lock and no atomic, and only taking the next block touches the heap.
// Nothing is freed one at a time. The arena hands out memory under one tag from begin() to
// end(), and the heap frees that tag once whoever reads the memory is done with it.
//
// A frame's memory looks like this, N being the frame number:
//
//     scratch.begin(heap_tag(Lifetime::Frame, N));
//     ... update and render allocate, from any worker ...
//     const HeapTag done = scratch.end();
//     tagged_heap.free(done); // after the submit, or a frame later if the GPU still reads it
//
// Fibers: a job that allocates reads its thread's slot through a call that is never inlined, so
// the slot is looked up again after every wait, and a job that resumed on another worker bumps
// that worker's cursor. An allocation itself never waits, which is why the cursor needs no lock.
// What an arena does not offer is a temporary mark to rewind to; a mark taken before a wait
// would name another thread's cursor after it.
//
// Nothing placed in an arena has its destructor run. A type whose destructor matters belongs on
// the heap or in a pool.

#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>
#include <ember/memory/tagged_heap.h>

#include <cstddef>
#include <memory_resource>

namespace ember
{
	class Arena final : public Allocator
	{
	public:
		// Threads that can allocate from arenas at once, one bit each in the slot mask.
		static constexpr u32 MAX_THREADS = 64;
		static constexpr u32 NO_SLOT = ~0u;

		// What the arena has handed out since begin(). It sums every thread's slot, so read it
		// after end(), once no thread is allocating.
		struct Stats
		{
			// What callers asked for, and what the arena holds: blocks times block size. The
			// difference is alignment padding and abandoned block tails.
			size_t bytes_requested = 0;
			size_t bytes_owned = 0;

			u32 blocks = 0;

			// Trips to the heap, oversized runs included, and slots that allocated at least once.
			u32 refills = 0;
			u32 threads = 0;
		};

		Arena() = default;
		~Arena() override;

		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		// Binds the arena to the heap it takes blocks from. Inactive until the first begin().
		void initialize(TaggedHeap& heap, const char* name);

		// Frees the current tag whether or not end() was called, and detaches from the heap.
		// For teardown only; a lifetime's turns go through begin(), end() and the heap.
		void shutdown();

		// Starts handing out memory under tag. The previous tag's blocks are left alone; whoever
		// consumes them frees them through the heap.
		void begin(HeapTag tag);

		// Stops handing out memory and returns the tag, for the heap to free now or later.
		HeapTag end();

		// Claims a slot for the calling thread before its first allocation from any arena. Claim
		// on the thread's way in and release on its way out, never mid frame. A thread that
		// already holds a slot keeps it. False when every slot is taken.
		[[nodiscard]] static bool register_thread();
		static void unregister_thread();

		// The calling thread's slot, NO_SLOT if it never claimed one. Never inlined: a compiler
		// may cache a thread local's address across a fiber switch, and the job that comes back
		// may be on another thread, so each allocation asks again through a call.
		static EMBER_NOINLINE u32 thread_slot();

		// The fast path, non-virtual so code that holds an Arena inlines it; containers reach the
		// same code through do_allocate(). A zero sized request takes a byte, so every pointer
		// handed out is unique until the tag is freed. The active check compiles out of release.
		[[nodiscard]] EMBER_FINLINE void* allocate(size_t size,
		    size_t alignment = DEFAULT_ALIGNMENT)
		{
			EMBER_ASSERT_MSG(m_active, "allocating from an arena between end() and begin()");
			EMBER_ASSERT(std::has_single_bit(alignment));

			Slot& slot = current_slot();
			const size_t bytes = size == 0 ? 1 : size;
			u8* aligned = align_up(slot.cursor, alignment);

			slot.bytes += bytes;

			// Aligning can walk past the end of the block, so the room left only means something
			// once the cursor is known to be inside it. An empty slot has both pointers null and
			// takes the same path, which is how a thread's first allocation gets its first block.
			if (aligned >= slot.end || static_cast<size_t>(slot.end - aligned) < bytes) [[unlikely]]
			{
				return refill(slot, bytes, alignment);
			}

			slot.cursor = aligned + bytes;
			return aligned;
		}

		// True if ptr lies in a block the current tag owns. A pointer from an earlier tag reads
		// false the moment the arena moves on, which is how a container that outlived its frame
		// is caught.
		bool owns(const void* ptr) const
		{
			return m_heap != nullptr && m_tag != NO_TAG && m_heap->tag_of(ptr) == m_tag;
		}

		bool is_active() const
		{
			return m_active;
		}

		HeapTag tag() const
		{
			return m_tag;
		}

		const char* name() const
		{
			return m_name;
		}

		Stats stats() const;

	private:
		// A cache line each: threads bump their cursors constantly and must not share a line.
		struct alignas(EMBER_CACHE_LINE) Slot
		{
			u8* cursor = nullptr;
			u8* end = nullptr;
			size_t bytes = 0;
			u32 refills = 0;
			u32 blocks = 0;
		};

		void* do_allocate(size_t bytes, size_t alignment) override;
		void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

		EMBER_FINLINE Slot& current_slot()
		{
			const u32 slot = thread_slot();
			EMBER_ASSERT(slot < MAX_THREADS && "arena memory belongs to registered threads");
			return m_slots[slot];
		}

		void* refill(Slot& slot, size_t bytes, size_t alignment);
		void forget_blocks();

		Slot m_slots[MAX_THREADS];
		TaggedHeap* m_heap = nullptr;
		HeapTag m_tag = NO_TAG;
		const char* m_name = "arena";
		bool m_active = false;
	};
}
