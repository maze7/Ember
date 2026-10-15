#pragma once

// General-purpose storage for allocations that need to be freed individually.
// Use an arena when a group of allocations shares a lifetime. The heap is
// intended for startup, tools, and libraries that need malloc-style storage;
// keep it off the frame's critical path.
//
// TLSF divides the reserved heap partition into reusable blocks. Each partition
// has its own spin mutex, so allocations on the same partition can contend.
// The normal TLSF path takes no more memory from the system after initialize().
//
// Every allocation has a sixteen-byte header containing its size, tag, and
// original block offset. free() reads these back, so callers need not remember
// them. Tracking level 1 adds counters, budget warnings, and profiler hooks.
// Level 2 also records source locations, numbers allocations, and links live
// blocks for leak reporting. A trailing guard and fill patterns help expose
// overwritten or uninitialized memory.
//
// Tools allocations use development memory when it is available. Failed retail
// allocations also try that partition, with a warning on the first attempt.
// Without development memory, a request that cannot fit calls out_of_memory().
//
// Direct calls capture the caller's source location. Containers and new_object
// pass an empty location rather than reporting a line inside the wrapper.
// At tracking level 2, break_on_allocation() can stop on a recorded number.

#include <ember/core/enum.h>
#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <source_location>

namespace ember::memory
{
	EMBER_ENUM_NAMES(Tag, "Unknown", "Engine", "Graphics", "Audio", "Physics", "Gameplay", "Assets",
	    "Scripting", "Network", "Platform", "Input", "Tools");

	// Allocates size bytes at the power-of-two alignment, charged to tag.
	// Failure calls out_of_memory() instead of returning null. A zero-size request
	// still gets its own block. where defaults to the caller's source location.
	// allocate_zeroed() also clears the payload.
	void* allocate(Tag tag, size_t size, size_t alignment = DEFAULT_ALIGNMENT,
	    std::source_location where = std::source_location::current());
	void* allocate_zeroed(Tag tag, size_t size, size_t alignment = DEFAULT_ALIGNMENT,
	    std::source_location where = std::source_location::current());

	// Resizes a block using DEFAULT_ALIGNMENT. Allocates a new block, copies the
	// bytes that fit, then frees the old one. A null ptr allocates under tag;
	// an existing block keeps its tag. Zero still allocates a block.
	void* reallocate(Tag tag, void* ptr, size_t size,
	    std::source_location where = std::source_location::current());

	// Frees a heap allocation. Null is harmless; the header supplies its tag and size.
	void free(void* ptr);

	// Returns the requested size, or zero for null. This excludes allocator overhead.
	size_t usable_size(const void* ptr);

	// Checks the header marker and trailing guard at tracking level 2.
	// Null passes, and lower tracking levels always return true.
	// ptr must otherwise name a live heap allocation.
	bool validate(const void* ptr);

	// Checks TLSF's bookkeeping while holding each partition's lock.
	// Intended for tests and diagnostics, since the walk can delay allocations.
	// Returns true with the system heap.
	bool check_heap();

	// Returns the shared Allocator object used by containers and object helpers.
	// The object exists for the whole run, but its backing storage belongs to
	// initialize() and shutdown().
	Allocator* heap();

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

	// Reports outstanding heap allocations and returns their count. Call after
	// owners have released their memory to use this as a leak report.
	// Level 2 lists individual blocks and available source locations; level 1
	// groups them by tag. Returns zero at tracking level 0.
	u32 report_leaks();

	// Requests a debugger break when allocation number is recorded.
	// Zero clears the request. Reproducing a number requires the same allocation
	// order, including across threads. Has no effect below tracking level 2.
	void break_on_allocation(u64 number);
}
