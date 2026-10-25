#pragma once

// Initialize memory before bringing up other engine systems. Use arenas for allocations that can
// be reclaimed together, and the heap for allocations that need to be freed individually.
//
// MemoryDef sets the budgets for the heap, tagged block pool, fiber stacks, and development memory.
// These partitions share one address range reserved at initialization. Normal allocation stays
// within those budgets; sanitizer builds use the system allocator for heap storage.
//
// Memory tracking can be controlled via the EMBER_MEMORY_TRACKING define (0 = none, 1 = counters,
// 2 = detailed).

#include <ember/core/enum.h>
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
	EMBER_ENUM_NAMES(Tag, "Unknown", "Engine", "Graphics", "Audio", "Physics", "Gameplay", "Assets",
	    "Scripting", "Network", "Platform", "Input", "Tools");

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

		// Capacity for fiber stacks and their guard pages. Zero disables stack allocation.
		u64 stack_capacity = 64_mb;

		// Optional heap for tools and retail overflow. Zero disables it.
		// Without development memory, Tools allocations use the retail heap.
		u64 development_capacity = EMBER_MEMORY_TRACKING >= 1 ? 256_mb : 0;
	};

	// Heap usage includes allocator overhead; block usage counts whole blocks.
	// These figures do not measure physical page residency.
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

	// Reserves the partitions and registers the calling thread for arena allocation.
	// Call before other memory use, with no concurrent memory API calls.
	// Logs setup failures and returns false.
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

	// Returns a page-aligned, committed range, or null if the partition is full.
	// Size rounds up to whole pages. The caller decommits each stack's guard page.
	u8* allocate_stacks(size_t size);

	// Returns ranges most recent first and recommits their guard pages for reuse.
	void free_stacks(u8* stacks, size_t size);

	// Reads partition usage and capacities for telemetry. Regions are sampled
	// separately, so activity during the read can affect the combined totals.
	// Serialize this with initialization and shutdown.
	MemoryMap map();

	// Heap usage thresholds in Tag order. Tracking builds warn once when a tag
	// reaches its budget; the warning does not reject the allocation.
	inline constexpr u64 TAG_BUDGETS[TAG_COUNT] = {
	    256_mb, // Unknown
	    64_mb,  // Engine
	    512_mb, // Graphics
	    64_mb,  // Audio
	    128_mb, // Physics
	    128_mb, // Gameplay
	    512_mb, // Assets
	    64_mb,  // Scripting
	    64_mb,  // Network
	    64_mb,  // Platform
	    64_mb,  // Input
	    64_mb,  // Tools
	};

	// Heap counters record requested bytes, excluding headers and allocator overhead.
	// arena_bytes sums each registered arena's requests since its last begin(),
	// including one byte for each zero-size request. Those counts remain until
	// the arena begins again. Arena requests do not count against the heap budget.
	struct TagStats
	{
		u64 current_bytes = 0;
		u64 peak_bytes = 0;
		u64 current_count = 0;
		u64 total_count = 0;
		u64 arena_bytes = 0;
	};

	// Reads one tag's counters, or the totals across all tags and registered arenas.
	// Concurrent allocations may change individual counters during the read.
	// Returns zeros at tracking level 0.
	TagStats stats(Tag tag);
	TagStats total();

	// Prints heap and arena usage by tag, followed by the partition totals.
	// Does nothing at tracking level 0.
	void report();

	// Reports outstanding heap allocations. Call after owners release their memory.
	// Level 2 lists individual blocks and available source locations; level 1
	// groups them by tag. Returns zero at tracking level 0.
	u32 report_leaks();

	// Requests a debugger break when allocation number is recorded.
	// Zero clears the request. Reproducing a number requires the same allocation
	// order, including across threads. Has no effect below tracking level 2.
	void break_on_allocation(u64 number);

	// Reports the failed request and stops the process. Heap and arena
	// allocation use this when they cannot provide the requested storage.
	[[noreturn]] void out_of_memory(size_t size, size_t alignment, const char* owner);
}
