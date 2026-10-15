#pragma once

// Legacy tracker declarations. Current tracking uses memory::Tag and is
// declared in heap.h; private hooks live in src/memory/internal.h.
// This header still describes the earlier Category interface.

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

	// Statistics for one category, or for the whole heap.
	Stats stats(Category category);
	Stats total();

	// Prints usage by category.
	void report();

	// Reports categories that still hold allocations. Call after their owners
	// have released memory when using the report to investigate leaks.
	u32 report_leaks();

	// Requests a debugger break on an allocation number. Zero clears the request.
	// Reproducing a number requires the same allocation order.
	void break_on_allocation(u64 number);
}

namespace ember::internal
{
// Hooks for the earlier category-based heap interface.
// Tracking level 0 provides empty inline definitions.
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
