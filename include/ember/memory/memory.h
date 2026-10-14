#pragma once

// The front of the memory module: initialization, the process-wide heaps, and the helpers that
// construct objects in them.
//
// Memory in Ember has one of two shapes. Anything with a lifetime, which is nearly everything a
// frame or a level produces, is placed in an Arena and freed by tag when that lifetime ends.
// Anything without one goes on the general Heap under a Category, attributed and budgeted but
// never on the frame's critical path. There is no third shape: no per allocation free in the
// main loop, and no allocation the module cannot name the owner of.

#include <ember/core/macros.h>
#include <ember/core/result.h>
#include <ember/core/types.h>
#include <ember/memory/allocator.h>
#include <ember/memory/arena.h>
#include <ember/memory/heap.h>
#include <ember/memory/tagged_heap.h>
#include <ember/memory/tracker.h>

#include <cstddef>
#include <new> // IWYU pragma: keep
#include <type_traits>
#include <utility>

namespace ember::memory
{
	struct MemoryDef
	{
		// Total capacity of the shared tagged block heap, rounded up to whole blocks.
		u64 block_heap_capacity = 256_mb;

		// Size of one block, which is one thread's bump region between refills. Must be a power of
		// two no larger than the heap. Larger blocks refill less often but abandon more tail space.
		u64 block_size = 2_mb;
	};

	// Brings up the memory services. The caller serializes this against shutdown(). On success the
	// calling thread holds an arena slot, and the Unknown heap is set as the default allocator for
	// any container that was not given one. Failure is logged. The reasons it can fail; a tracker
	// that would not start, a block heap the OS would not reserve, no thread slot left, are all
	// configuration or environment, so the caller has nothing to branch on beyond the bool.
	bool initialize(const MemoryDef& def = {});

	// Releases the arenas and the block heap. Every job and every thread using engine memory needs
	// to have stopped first; pointers into either are invalid afterwards. The default allocator
	// stays attached to the process heap so that static destruction can still free what it owns.
	void shutdown();

	// Per-thread allocator state, for threads the job system does not own. A thread releases its
	// thread-local heap allocations before shutdown_thread().
	bool initialize_thread();
	void shutdown_thread();

	// The shared tagged heap that block-based transient allocators refill from. Valid from
	// initialize() to shutdown().
	TaggedHeap& tagged_heap();

	// The heap view for a category. Valid at any time, before initialize and after shutdown
	// included, which is what lets a static object use it.
	Heap* heap(Category category = Category::Unknown);

	// Constructs a T in [allocator]'s memory. Construction must not throw and allocation failure
	// never returns, so the result is always an object. Destroy it with delete_object through the
	// same resource.
	template <typename T, typename... Args>
	T* new_object(Allocator* allocator, Args&&... args)
	{
		static_assert(std::is_nothrow_constructible_v<T, Args&&...>,
		    "construction must not throw; fallible setup belongs in a create() function");

		void* memory = allocator->allocate(sizeof(T), alignof(T));
		return ::new (memory) T(std::forward<Args>(args)...);
	}

	// Destroys an object and returns its storage. ptr may be null, and must otherwise point
	// at the most derived T, because the size and alignment handed back are T's.
	template <typename T>
	void delete_object(Allocator* allocator, T* ptr)
	{
		static_assert(std::is_nothrow_destructible_v<T>, "destructors must not throw");

		if (ptr == nullptr)
		{
			return;
		}

		ptr->~T();
		allocator->deallocate(ptr, sizeof(T), alignof(T));
	}

	// Reports an allocation failure and ends the process. Engine allocators call this rather than
	// returning null or throwing, because there's nothing sensible we can do when we OOM.
	[[noreturn]] void out_of_memory(size_t size, size_t alignment, const char* owner);
}
