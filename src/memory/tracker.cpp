#include <ember/core/enum.h>
#include <ember/core/profile.h>
#include <ember/memory/tracker.h>

#include <atomic>
#include <cstdio>

namespace ember::memory_tracker
{
	namespace
	{
		constexpr size_t CATEGORY_COUNT = (size_t)Category::Count;

#if EMBER_MEMORY_TRACKING >= 1
		// A cache line each, so categories busy on different threads do not share one.
		struct alignas(EMBER_CACHE_LINE) Counters
		{
			std::atomic<u64> current_bytes{0};
			std::atomic<u64> peak_bytes{0};
			std::atomic<u64> current_count{0};
			std::atomic<u64> total_count{0};
		};

		constinit Counters s_counters[CATEGORY_COUNT];
		constinit Counters s_total;
		constinit std::atomic<bool> s_budget_warned[CATEGORY_COUNT];
		constinit std::atomic<u64> s_next_number{1};
		constinit std::atomic<u64> s_break_number{0};

		Stats read(const Counters& counters)
		{
			return Stats{
			    .current_bytes = counters.current_bytes.load(std::memory_order_relaxed),
			    .peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed),
			    .current_count = counters.current_count.load(std::memory_order_relaxed),
			    .total_count = counters.total_count.load(std::memory_order_relaxed),
			};
		}

		// Returns the bytes held before this allocation, for the budget check.
		u64 add(Counters& counters, u64 size)
		{
			const u64 previous = counters.current_bytes.fetch_add(size, std::memory_order_relaxed);
			counters.current_count.fetch_add(1, std::memory_order_relaxed);
			counters.total_count.fetch_add(1, std::memory_order_relaxed);

			const u64 now = previous + size;
			u64 peak = counters.peak_bytes.load(std::memory_order_relaxed);
			while (now > peak &&
			       !counters.peak_bytes.compare_exchange_weak(peak, now, std::memory_order_relaxed))
			{
			}

			return previous;
		}

		void subtract(Counters& counters, u64 size)
		{
			counters.current_bytes.fetch_sub(size, std::memory_order_relaxed);
			counters.current_count.fetch_sub(1, std::memory_order_relaxed);
		}

		void print_row(const char* name, const Stats& stats, u64 budget)
		{
			std::fprintf(stderr, "%-10s %14llu %14llu %10llu %12llu %14llu\n", name,
			    (unsigned long long)stats.current_bytes, (unsigned long long)stats.peak_bytes,
			    (unsigned long long)stats.current_count, (unsigned long long)stats.total_count,
			    (unsigned long long)budget);
		}
#endif
	}

	Stats stats(Category category)
	{
#if EMBER_MEMORY_TRACKING >= 1
		return read(s_counters[(size_t)category]);
#else
		(void)category;
		return {};
#endif
	}

	Stats total()
	{
#if EMBER_MEMORY_TRACKING >= 1
		return read(s_total);
#else
		return {};
#endif
	}

	void report()
	{
#if EMBER_MEMORY_TRACKING >= 1
		std::fprintf(stderr, "%-10s %14s %14s %10s %12s %14s\n", "Category", "Current", "Peak",
		    "Live", "Total", "Budget");

		for (size_t index = 0; index < CATEGORY_COUNT; ++index)
		{
			const Category category = (Category)index;
			print_row(enum_name(category), read(s_counters[index]), CATEGORY_BUDGETS[index]);
		}

		print_row("Total", read(s_total), 0);
#endif
	}

	u32 report_leaks()
	{
		u32 leaked = 0;

#if EMBER_MEMORY_TRACKING >= 1
		for (size_t index = 0; index < CATEGORY_COUNT; ++index)
		{
			const Stats stats = read(s_counters[index]);
			if (stats.current_count == 0)
			{
				continue;
			}

			std::fprintf(stderr, "[memory] %s still holds %llu bytes in %llu allocations\n",
			    enum_name((Category)index), (unsigned long long)stats.current_bytes,
			    (unsigned long long)stats.current_count);
			++leaked;
		}
#endif

		return leaked;
	}

	void break_on_allocation(u64 number)
	{
#if EMBER_MEMORY_TRACKING >= 2
		s_break_number.store(number, std::memory_order_relaxed);
#else
		(void)number;
#endif
	}
}

#if EMBER_MEMORY_TRACKING >= 1
namespace ember::internal
{
	void track_allocate(void* ptr, size_t size, Category category)
	{
		using namespace memory_tracker;

		const size_t index = (size_t)category;
		EMBER_PROFILE_ALLOC(ptr, size, enum_name(category));
		(void)ptr;

		add(s_total, size);
		const u64 previous = add(s_counters[index], size);

		// A budget is reported the first time it is crossed and never again, so the log says it
		// once rather than on every allocation past the line.
		const u64 budget = CATEGORY_BUDGETS[index];
		if (previous < budget && previous + size >= budget &&
		    !s_budget_warned[index].exchange(true, std::memory_order_relaxed))
		{
			std::fprintf(stderr, "[memory] %s crossed its budget of %llu bytes\n",
			    enum_name(category), (unsigned long long)budget);
		}

	#if EMBER_MEMORY_TRACKING >= 2
		const u64 number = s_next_number.fetch_add(1, std::memory_order_relaxed);
		if (number == s_break_number.load(std::memory_order_relaxed)) [[unlikely]]
		{
			std::fprintf(stderr, "[memory] breaking on allocation %llu\n",
			    (unsigned long long)number);
			EMBER_DEBUG_BREAK();
		}
	#endif
	}

	void track_free(void* ptr, size_t size, Category category)
	{
		using namespace memory_tracker;

		EMBER_PROFILE_FREE(ptr, enum_name(category));
		(void)ptr;

		subtract(s_total, size);
		subtract(s_counters[(size_t)category], size);
	}
}
#endif
