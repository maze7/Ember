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
// pass an empty location rather than reporting a line inside the wrapper. The
// counts, budgets and leak report built from these records are in memory.h.

#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <source_location>

namespace ember::memory
{
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
}
