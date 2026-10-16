#pragma once

// Owns the memory reservation and the allocators built over it. Callers that
// only need heap allocation or object construction can include heap.h or
// new.h directly.
//
// At startup, we reserve one address range for the heap, the block pool, fiber
// stacks, and optional development memory. The normal allocator backend works
// within these partitions, making their capacities explicit memory budgets.
// Sanitizer builds use the system allocator for heap storage.
//
// Use an arena when allocations can be reclaimed together, such as the data
// produced for a frame or level. Use the heap when allocations need to be
// released individually.
//
// Allocations must stay within the module's lifetime. In particular, static
// objects must not retain heap storage past shutdown().
//
// Allocation sizes use size_t to match the standard library's allocator
// interface and the platform memory APIs.

#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/allocator.h>
#include <ember/memory/arena.h>
#include <ember/memory/block_heap.h>
#include <ember/memory/heap.h>
#include <ember/memory/new.h>
#include <ember/memory/tag.h>

#include <cstddef>

namespace ember::memory
{
	struct MemoryDef
	{
		// Capacity of the retail heap. Requests that cannot fit try development
		// memory when it is available; otherwise allocation fails.
		u64 heap_capacity = 256_mb;

		// Capacity shared by all block lifetimes, rounded up to whole blocks.
		u64 block_capacity = 256_mb;

		// Block size used for arena refills. Must be a power of two.
		// Larger blocks reduce refills but can leave more space unused.
		u64 block_size = 2_mb;

		// Capacity for fiber stacks and their guard pages. The job system takes
		// its stacks from here when it stats; the default holds 160 fibers of
		// 256 KiB with room to spare. Zero leaves the job system no stacks.
		u64 stack_capacity = 64_mb;

		// Optional heap for tools and retail overflow. Zero disables it.
		// Without development memory, Tools allocations use the retail heap.
		u64 development_capacity = EMBER_MEMORY_TRACKING >= 1 ? 256_mb : 0;
	};

	// Allocator usage for one region. Heap usage includes per-allocation
	// overhead; block usage counts whole blocks. These figures describe
	// allocator usage rather than physical page residency.
	struct RegionStats
	{
		u64 used_bytes = 0;
		u64 peak_bytes = 0;
		u64 capacity = 0;
	};

	// Usage across the retail heap, block pool, fiber stacks, and development heap.
	// overflowed records that a retail allocation needed development fallback.
	// retail_peak adds the separate heap and block peaks, which may have
	// occurred at different times.
	struct MemoryMap
	{
		RegionStats heap;
		RegionStats blocks;
		RegionStats stacks;
		RegionStats development;

		u64 retail_bytes = 0;
		u64 retail_peak = 0;
		u64 retail_capacity = 0;
		bool overflowed = false;
	};

	// Reserves the memory map, initializes its allocators, and registers the
	// calling thread for arena allocation. Returns false if setup fails.
	// Call before other memory use, with no concurrent memory API calls.
	// Initialization failures are logged.
	bool initialize(const MemoryDef& def = {});

	// Reports outstanding heap allocations, then releases the reservation.
	// All users must have stopped, arena lifetimes must have been reclaimed,
	// and owners must have released their heap allocations before this call.
	void shutdown();

	// Registers or unregisters the calling thread for arena allocation.
	// External threads register before their first arena use and unregister
	// after their last. Registration fails when all slots are occupied.
	bool initialize_thread();
	void shutdown_thread();

	// The shared block heap used by arenas between initialize() and shutdown().
	BlockHeap* block_heap();

	// Takes [size] bytes from the stack partition, rounded up to whole pages. The
	// range is page-aligned and committed; the caller lays out its stacks and decommits
	// the guard page below each one. Returns null when the partition cannot hold the
	// request.
	u8* allocate_stacks(size_t size);

	// Returns a range from allocate_stacks(), most recent first, and commits it
	// again, guard pages included, so the next caller starts from the same state.
	void free_stacks(u8* stacks, size_t size);

	// Reads partition usage and capacities for telemetry. Regions are sampled
	// separately, so activity during the read can affect the combined totals.
	// Serialize this with initialization and shutdown.
	MemoryMap map();

	// Reports the failed request and stops the process. Heap and arena
	// allocation use this when they cannot provide the requested storage.
	[[noreturn]] void out_of_memory(size_t size, size_t alignment, const char* owner);
}
