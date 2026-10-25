#include <ember/core/enum.h>
#include <ember/core/profile.h>
#include <ember/core/spin_mutex.h>
#include <ember/memory/arena.h>
#include <ember/memory/heap.h>
#include <ember/memory/memory.h>

#include "internal.h"

#include <atomic>
#include <cstdio>

namespace ember::memory
{
	namespace
	{
#if EMBER_MEMORY_TRACKING >= 1
		// Separate tags by cache lines so updates to unrelated tags do not invalidate
		// the same line.
		struct alignas(EMBER_CACHE_LINE) Counters
		{
			std::atomic<u64> current_bytes{0};
			std::atomic<u64> peak_bytes{0};
			std::atomic<u64> current_count{0};
			std::atomic<u64> total_count{0};
		};

		constinit Counters s_counters[TAG_COUNT];
		constinit Counters s_total;
		constinit std::atomic<bool> s_budget_warned[TAG_COUNT];

		TagStats read(const Counters& counters)
		{
			return TagStats{
			    .current_bytes = counters.current_bytes.load(std::memory_order_relaxed),
			    .peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed),
			    .current_count = counters.current_count.load(std::memory_order_relaxed),
			    .total_count = counters.total_count.load(std::memory_order_relaxed),
			};
		}

		// Return the previous byte count so the caller can tell whether this
		// allocation reached a budget.
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

		void print_row(const char* name, const TagStats& stats, u64 budget)
		{
			std::fprintf(stderr, "%-10s %14llu %14llu %10llu %12llu %14llu %14llu\n", name,
			    (unsigned long long)stats.current_bytes, (unsigned long long)stats.peak_bytes,
			    (unsigned long long)stats.current_count, (unsigned long long)stats.total_count,
			    (unsigned long long)stats.arena_bytes, (unsigned long long)budget);
		}

		void print_region(const char* name, const RegionStats& region)
		{
			std::fprintf(stderr, "%-12s %14llu %14llu %14llu\n", name,
			    (unsigned long long)region.used_bytes, (unsigned long long)region.peak_bytes,
			    (unsigned long long)region.capacity);
		}

		// Fixed storage keeps arena registration from allocating.
		constexpr u32 MAX_ARENAS = 32;
		constinit SpinMutex s_arena_lock;
		constinit const Arena* s_arenas[MAX_ARENAS];
		constinit u32 s_arena_count = 0;

		u64 arena_bytes(Tag tag)
		{
			u64 bytes = 0;

			s_arena_lock.lock();
			for (u32 index = 0; index < s_arena_count; ++index)
			{
				bytes += s_arenas[index]->tag_bytes(tag);
			}
			s_arena_lock.unlock();

			return bytes;
		}

		u64 arena_bytes_total()
		{
			u64 bytes = 0;

			s_arena_lock.lock();
			for (u32 index = 0; index < s_arena_count; ++index)
			{
				for (size_t tag = 0; tag < TAG_COUNT; ++tag)
				{
					bytes += s_arenas[index]->tag_bytes((Tag)tag);
				}
			}
			s_arena_lock.unlock();

			return bytes;
		}

#endif

#if EMBER_MEMORY_TRACKING >= 2
		// Link live blocks through their debug headers so a leak report can describe
		// each allocation. The lock protects the links during allocation and free.
		constinit SpinMutex s_list_lock;
		constinit internal::Header* s_list_head = nullptr;
		constinit std::atomic<u64> s_next_number{1};
		constinit std::atomic<u64> s_break_number{0};

		internal::DebugHeader* debug_of(internal::Header* header)
		{
			return (internal::DebugHeader*)((u8*)header - sizeof(internal::DebugHeader));
		}
#endif
	}

	TagStats stats(Tag tag)
	{
#if EMBER_MEMORY_TRACKING >= 1
		TagStats stats = read(s_counters[(size_t)tag]);
		stats.arena_bytes = arena_bytes(tag);
		return stats;
#else
		(void)tag;
		return {};
#endif
	}

	TagStats total()
	{
#if EMBER_MEMORY_TRACKING >= 1
		TagStats stats = read(s_total);
		stats.arena_bytes = arena_bytes_total();
		return stats;
#else
		return {};
#endif
	}

	void report()
	{
#if EMBER_MEMORY_TRACKING >= 1
		std::fprintf(stderr, "%-10s %14s %14s %10s %12s %14s %14s\n", "Tag", "Heap", "Peak", "Live",
		    "Total", "Arena", "Budget");

		for (size_t index = 0; index < TAG_COUNT; ++index)
		{
			print_row(enum_name((Tag)index), stats((Tag)index), TAG_BUDGETS[index]);
		}

		print_row("Total", total(), 0);

		const MemoryMap memory_map = map();
		std::fprintf(stderr, "\n%-12s %14s %14s %14s\n", "Region", "Used", "Peak", "Capacity");
		print_region("Heap", memory_map.heap);
		print_region("Blocks", memory_map.blocks);
		print_region("Stacks", memory_map.stacks);
		if (memory_map.development.capacity != 0)
		{
			print_region("Development", memory_map.development);
		}

		std::fprintf(stderr, "%-12s %14llu %14llu %14llu%s\n", "Retail",
		    (unsigned long long)memory_map.retail_bytes, (unsigned long long)memory_map.retail_peak,
		    (unsigned long long)memory_map.retail_capacity,
		    memory_map.overflowed ? "   spilled into development memory" : "");
#endif
	}

	u32 report_leaks()
	{
		u32 leaked = 0;

#if EMBER_MEMORY_TRACKING >= 2
		// Container and object helpers may have no source location.
		s_list_lock.lock();

		for (internal::Header* header = s_list_head; header != nullptr;
		    header = debug_of(header)->next)
		{
			const internal::DebugHeader* debug = debug_of(header);
			if (debug->file != nullptr && debug->file[0] != '\0')
			{
				std::fprintf(stderr, "[memory] leak: %s, %llu bytes, allocation %u, %s:%u\n",
				    enum_name((Tag)header->tag), (unsigned long long)header->size, debug->number,
				    debug->file, debug->line);
			}
			else
			{
				std::fprintf(stderr,
				    "[memory] leak: %s, %llu bytes, allocation %u, no source line\n",
				    enum_name((Tag)header->tag), (unsigned long long)header->size, debug->number);
			}

			++leaked;
		}

		s_list_lock.unlock();
#elif EMBER_MEMORY_TRACKING >= 1
		for (size_t index = 0; index < TAG_COUNT; ++index)
		{
			const TagStats stats = read(s_counters[index]);
			if (stats.current_count == 0)
			{
				continue;
			}

			std::fprintf(stderr, "[memory] %s still holds %llu bytes in %llu allocations\n",
			    enum_name((Tag)index), (unsigned long long)stats.current_bytes,
			    (unsigned long long)stats.current_count);
			leaked += (u32)stats.current_count;
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

#if EMBER_MEMORY_TRACKING >= 1
	void internal::track_allocate(Header* header)
	{
		const Tag tag = (Tag)header->tag;
		const size_t index = (size_t)tag;
		const u64 size = header->size;
		void* payload = (u8*)header + sizeof(Header);

		EMBER_PROFILE_ALLOC(payload, size, enum_name(tag));
		(void)payload;

		add(s_total, size);
		const u64 previous = add(s_counters[index], size);

		// Warn once per tag so later allocations do not flood the log.
		const u64 budget = TAG_BUDGETS[index];
		if (previous < budget && previous + size >= budget &&
		    !s_budget_warned[index].exchange(true, std::memory_order_relaxed))
		{
			std::fprintf(stderr, "[memory] %s crossed its budget of %llu bytes\n", enum_name(tag),
			    (unsigned long long)budget);
		}

	#if EMBER_MEMORY_TRACKING >= 2
		DebugHeader* debug = debug_of(header);
		debug->number = (u32)s_next_number.fetch_add(1, std::memory_order_relaxed);

		s_list_lock.lock();
		debug->prev = nullptr;
		debug->next = s_list_head;
		if (s_list_head != nullptr)
		{
			debug_of(s_list_head)->prev = header;
		}
		s_list_head = header;
		s_list_lock.unlock();

		if (debug->number == s_break_number.load(std::memory_order_relaxed)) [[unlikely]]
		{
			std::fprintf(stderr, "[memory] breaking on allocation %u\n", debug->number);
			EMBER_DEBUG_BREAK();
		}
	#endif
	}

	void internal::track_free(Header* header)
	{
		const Tag tag = (Tag)header->tag;
		void* payload = (u8*)header + sizeof(Header);

		EMBER_PROFILE_FREE(payload, enum_name(tag));
		(void)payload;

		subtract(s_total, header->size);
		subtract(s_counters[(size_t)tag], header->size);

	#if EMBER_MEMORY_TRACKING >= 2
		DebugHeader* debug = debug_of(header);

		s_list_lock.lock();
		if (debug->prev != nullptr)
		{
			debug_of(debug->prev)->next = debug->next;
		}
		else
		{
			s_list_head = debug->next;
		}
		if (debug->next != nullptr)
		{
			debug_of(debug->next)->prev = debug->prev;
		}
		s_list_lock.unlock();
	#endif
	}

	void internal::register_arena(const Arena* arena)
	{
		bool full = false;

		s_arena_lock.lock();
		if (s_arena_count < MAX_ARENAS)
		{
			s_arenas[s_arena_count++] = arena;
		}
		else
		{
			full = true;
		}
		s_arena_lock.unlock();

		EMBER_ASSERT_MSG(!full,
		    "more arenas than the tracker watches: raise MAX_ARENAS or make fewer");
	}

	void internal::unregister_arena(const Arena* arena)
	{
		s_arena_lock.lock();
		for (u32 index = 0; index < s_arena_count; ++index)
		{
			if (s_arenas[index] == arena)
			{
				s_arenas[index] = s_arenas[--s_arena_count];
				break;
			}
		}
		s_arena_lock.unlock();
	}

#endif
}
