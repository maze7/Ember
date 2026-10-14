#pragma once

// What the heap records about its traffic, by category: bytes and allocations held now, at
// their peak and in total, against the budget each category was given. The counters are
// relaxed atomics bumped on every heap allocation and free, which is affordable because the
// heap is off the frame's critical path; everything on it goes through an arena, whose stats
// are its own.
//
// There is no table of live allocations. Each Heap view frees exactly what it allocated, so the
// per category counters balance by themselves, and a category not at zero when the module shuts
// down has leaked. Tracy gets every allocation and free under the category's name, so a leak's
// history is in the capture.
//
// The level is EMBER_MEMORY_TRACKING, decided in macros.h. Everything here compiles to nothing
// at level 0.

#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/heap.h>

#include <cstddef>

namespace ember::memory_tracker
{
	struct Stats
	{
		u64 current_bytes = 0;
		u64 peak_bytes = 0;
		u64 current_count = 0;
		u64 total_count = 0;
	};

	// The stats for one category, and for the whole heap. Zeros at level 0.
	Stats stats(Category category);
	Stats total();

	// Prints a table of every category.
	void report();

	// Prints every category still holding memory and returns how many there are; at shutdown,
	// that is the leak report. Zero at level 0.
	u32 report_leaks();

	// Breaks into the debugger when the heap allocation with this number is made; zero clears
	// it. Numbers count heap allocations from the start of the run, so a number names the same
	// allocation again as long as the run does the same work. Level 2 only.
	void break_on_allocation(u64 number);
}

namespace ember::internal
{
// The heap's hooks, called with the size the heap actually set aside. At level 0 they are
// empty inline functions, so nothing is left in the heap's code.
#if EMBER_MEMORY_TRACKING >= 1
	void track_allocate(void* ptr, size_t size, Category category);
	void track_free(void* ptr, size_t size, Category category);
#else
	inline void track_allocate(void*, size_t, Category)
	{
	}

	inline void track_free(void*, size_t, Category)
	{
	}
#endif
}
