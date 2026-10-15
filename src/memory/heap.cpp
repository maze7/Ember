#include <ember/core/enum.h>
#include <ember/core/spin_mutex.h>
#include <ember/memory/heap.h>
#include <ember/memory/memory.h>

#include "internal.h"
#include "tlsf.h"

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>

// Use malloc for backing storage so sanitizers can observe the allocations.
// Our headers and tracking still apply, but the reserved heap partitions
// are unused and report zero usage.
#ifndef EMBER_USE_SYSTEM_HEAP
	#define EMBER_USE_SYSTEM_HEAP 0
#endif

#if EMBER_USE_SYSTEM_HEAP
	#include <cstdlib>
#endif

namespace ember::memory
{
	namespace
	{
		// One TLSF allocator and its partition. used counts allocated TLSF blocks,
		// including their per-block overhead. The lock protects both the allocator
		// and its usage counters.
		struct Region
		{
			SpinMutex lock;
			tlsf_t tlsf = nullptr;
			u8* base = nullptr;
			u64 capacity = 0;
			u64 used = 0;
			u64 peak = 0;

			bool owns(const void* ptr) const
			{
				const u8* p = (const u8*)ptr;
				return base != nullptr && p >= base && p < base + capacity;
			}
		};

		constinit Region s_retail;
		constinit Region s_development;
		constinit std::atomic<bool> s_overflowed{false};

		// TLSF's guaranteed alignment. Stronger payload alignment is handled inside
		// the raw block.
		constexpr size_t RAW_ALIGNMENT = 8;

#if EMBER_MEMORY_TRACKING >= 2
		constexpr size_t HEADER_BYTES = sizeof(internal::DebugHeader) + sizeof(internal::Header);
		constexpr size_t GUARD_BYTES = sizeof(u64);
#else
		constexpr size_t HEADER_BYTES = sizeof(internal::Header);
		constexpr size_t GUARD_BYTES = 0;
#endif

		static_assert(HEADER_BYTES % DEFAULT_ALIGNMENT == 0,
		    "a payload that needs no shifting must land on the default alignment");

		internal::Header* header_of(const void* payload)
		{
			return (internal::Header*)((const u8*)payload - sizeof(internal::Header));
		}

		// Place TLSF's tables first, then divide the remaining partition into pools
		// it can address. An allocation must fit within one pool, even when other
		// pools have room. A zero-size partition stays unused.
		bool initialize_region(Region& region, void* base, size_t size)
		{
			if (size == 0)
			{
				return true;
			}

#if !EMBER_USE_SYSTEM_HEAP
			const size_t smallest_pool = tlsf_pool_overhead() + tlsf_block_size_min();
			if (size < tlsf_size() + smallest_pool)
			{
				return false;
			}

			region.tlsf = tlsf_create(base);
			if (region.tlsf == nullptr)
			{
				return false;
			}

			u8* cursor = (u8*)base + tlsf_size();
			u8* end = (u8*)base + size;
			while (cursor < end)
			{
				const size_t remaining = (size_t)(end - cursor);
				const size_t chunk =
				    remaining < tlsf_block_size_max() ? remaining : tlsf_block_size_max();
				if (chunk < smallest_pool)
				{
					break;
				}

				if (tlsf_add_pool(region.tlsf, cursor, chunk) == nullptr)
				{
					return false;
				}

				cursor += chunk;
			}
#endif

			region.base = (u8*)base;
			region.capacity = size;
			region.used = 0;
			region.peak = 0;
			return true;
		}

		void shutdown_region(Region& region)
		{
#if !EMBER_USE_SYSTEM_HEAP
			if (region.tlsf != nullptr)
			{
				tlsf_destroy(region.tlsf);
			}
#endif

			region.tlsf = nullptr;
			region.base = nullptr;
			region.capacity = 0;
			region.used = 0;
			region.peak = 0;
		}

		// Allocate backing storage, updating partition usage on the TLSF path.
		// Return null when the request cannot be satisfied so the caller can try
		// development memory.
		void* take(Region& region, size_t bytes)
		{
#if EMBER_USE_SYSTEM_HEAP
			(void)region;
			return std::malloc(bytes);
#else
			if (region.tlsf == nullptr)
			{
				return nullptr;
			}

			region.lock.lock();
			void* raw = tlsf_malloc(region.tlsf, bytes);
			if (raw != nullptr)
			{
				region.used += tlsf_block_size(raw) + tlsf_alloc_overhead();
				if (region.used > region.peak)
				{
					region.peak = region.used;
				}
			}
			region.lock.unlock();

			return raw;
#endif
		}

		// Read the block size before freeing it: TLSF may merge it with a neighbour
		// during the free, and the merged size would no longer describe this allocation.
		void give_back(void* raw)
		{
#if EMBER_USE_SYSTEM_HEAP
			std::free(raw);
#else
			Region& region = s_retail.owns(raw) ? s_retail : s_development;
			EMBER_ASSERT_MSG(region.owns(raw), "free of a pointer that lies outside the heap");

			region.lock.lock();
			region.used -= tlsf_block_size(raw) + tlsf_alloc_overhead();
			tlsf_free(region.tlsf, raw);
			region.lock.unlock();
#endif
		}

		void read(Region& region, RegionStats& stats)
		{
			region.lock.lock();
			stats.used_bytes = region.used;
			stats.peak_bytes = region.peak;
			stats.capacity = region.capacity;
			region.lock.unlock();
		}
	}

	void* allocate(Tag tag, size_t size, size_t alignment, std::source_location where)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));

		if (s_retail.base == nullptr) [[unlikely]]
		{
			internal::fatal("heap allocation before memory::initialize()");
		}

		// Leave enough room to align the payload after the headers. The header stays
		// immediately before the payload; base_offset lets free() recover the raw block
		// despite any padding. Tracking level 2 adds a debug header before it and a
		// guard after the payload.
		const size_t slack = alignment > RAW_ALIGNMENT ? alignment - RAW_ALIGNMENT : 0;
		const size_t bytes = HEADER_BYTES + slack + size + GUARD_BYTES;

		// Tools use development memory first. Retail failures try it too, so an
		// oversized scene can still be inspected. Warn on the first fallback attempt
		// to make the retail shortfall visible.
		const bool has_development = s_development.base != nullptr;
		Region* region = tag == Tag::Tools && has_development ? &s_development : &s_retail;
		u8* raw = (u8*)take(*region, bytes);
		if (raw == nullptr && region == &s_retail && has_development) [[unlikely]]
		{
			if (!s_overflowed.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				    "[memory] the retail heap of %llu bytes is full: spilling into development "
				    "memory, this build would not fit a shipping machine\n",
				    (unsigned long long)s_retail.capacity);
			}

			raw = (u8*)take(s_development, bytes);
		}

		if (raw == nullptr) [[unlikely]]
		{
			out_of_memory(size, alignment, enum_name(tag));
		}

		u8* payload = align_up(raw + HEADER_BYTES, alignment);
		internal::Header* header = header_of(payload);
		header->size = size;
		header->base_offset = (u32)(payload - raw);
		header->tag = (u8)tag;
		header->reserved = 0;
		header->magic = internal::HEADER_MAGIC;

#if EMBER_MEMORY_TRACKING >= 2
		internal::DebugHeader* debug =
		    (internal::DebugHeader*)((u8*)header - sizeof(internal::DebugHeader));
		debug->file = where.file_name();
		debug->line = where.line();
		debug->prev = nullptr;
		debug->next = nullptr;
		debug->number = 0;
		std::memcpy(payload + size, &internal::TRAILING_GUARD, sizeof(internal::TRAILING_GUARD));

		// A recognizable fill makes unwritten memory easier to spot than values
		// left by an old object.
		std::memset(payload, 0xCD, size);
#else
		(void)where;
#endif

		internal::track_allocate(header);
		return payload;
	}

	void* allocate_zeroed(Tag tag, size_t size, size_t alignment, std::source_location where)
	{
		void* ptr = allocate(tag, size, alignment, where);
		std::memset(ptr, 0, size);
		return ptr;
	}

	void* reallocate(Tag tag, void* ptr, size_t size, std::source_location where)
	{
		// Rebuild the allocation so its alignment and headers follow the normal path.
		// An existing block keeps its tag; tag is only used when ptr is null.
		if (ptr == nullptr)
		{
			return allocate(tag, size, DEFAULT_ALIGNMENT, where);
		}

		const internal::Header* header = header_of(ptr);
		EMBER_ASSERT_MSG(header->magic == internal::HEADER_MAGIC,
		    "reallocate of a pointer the heap did not hand out");

		void* moved = allocate((Tag)header->tag, size, DEFAULT_ALIGNMENT, where);
		std::memcpy(moved, ptr, header->size < size ? header->size : size);
		free(ptr);
		return moved;
	}

	void free(void* ptr)
	{
		if (ptr == nullptr)
		{
			return;
		}

		if (s_retail.base == nullptr) [[unlikely]]
		{
			internal::fatal("heap free after memory::shutdown()");
		}

		internal::Header* header = header_of(ptr);
		EMBER_ASSERT_MSG(header->magic == internal::HEADER_MAGIC,
		    "free of a pointer the heap did not hand out, or whose header was overwritten");
		EMBER_ASSERT_MSG(validate(ptr), "the block was written past its end");

		internal::track_free(header);

#if EMBER_MEMORY_TRACKING >= 2
		// Leave a recognizable pattern for debugging stale pointers. The header marker
		// is cleared below before returning the block. Neither check can reliably
		// identify a stale pointer once the storage has been reused.
		std::memset(ptr, 0xDD, header->size);
#endif

		u8* raw = (u8*)ptr - header->base_offset;
		header->magic = 0;
		give_back(raw);
	}

	size_t usable_size(const void* ptr)
	{
		return ptr == nullptr ? 0 : header_of(ptr)->size;
	}

	bool validate(const void* ptr)
	{
#if EMBER_MEMORY_TRACKING >= 2
		if (ptr == nullptr)
		{
			return true;
		}

		const internal::Header* header = header_of(ptr);
		u64 guard = 0;
		std::memcpy(&guard, (const u8*)ptr + header->size, sizeof(guard));
		return header->magic == internal::HEADER_MAGIC && guard == internal::TRAILING_GUARD;
#else
		(void)ptr;
		return true;
#endif
	}

	bool check_heap()
	{
#if EMBER_USE_SYSTEM_HEAP
		return true;
#else
		bool intact = true;

		for (Region* region : {&s_retail, &s_development})
		{
			if (region->tlsf == nullptr)
			{
				continue;
			}

			region->lock.lock();
			intact = tlsf_check(region->tlsf) == 0 && intact;
			region->lock.unlock();
		}

		return intact;
#endif
	}

	namespace
	{
		// The shared Allocator object for heap clients. Tags arrive with each request,
		// so callers can charge different subsystems without keeping a separate
		// allocator object for each one.
		class HeapAllocator final : public Allocator
		{
		public:
			constexpr HeapAllocator() = default;

			void* allocate(Tag tag, size_t size, size_t alignment,
			    std::source_location where) override
			{
				return allocate(tag, size, alignment, where);
			}

			void deallocate(void* ptr, size_t, size_t) override
			{
				free(ptr);
			}
		};

		// Keep the Allocator object alive through static teardown. The empty union
		// destructor skips the member's destructor, allowing late frees to reach
		// the heap's shutdown check.
		union HeapHolder
		{
			HeapAllocator heap;

			constexpr HeapHolder() : heap()
			{
			}

			~HeapHolder()
			{
			}
		};

		constinit HeapHolder s_holder;
	}

	Allocator* heap()
	{
		return &s_holder.heap;
	}

	bool internal::heap_initialize(void* retail, size_t retail_size, void* development,
	    size_t development_size)
	{
		EMBER_ASSERT_MSG(s_retail.base == nullptr, "the heap is initialized once");
#if !EMBER_USE_SYSTEM_HEAP
		EMBER_ASSERT_MSG(tlsf_align_size() == RAW_ALIGNMENT,
		    "the heap lays its blocks out for an allocator that aligns to eight bytes");
#endif

		s_overflowed.store(false, std::memory_order_relaxed);

		if (!initialize_region(s_retail, retail, retail_size))
		{
			return false;
		}

		if (!initialize_region(s_development, development, development_size))
		{
			shutdown_region(s_retail);
			return false;
		}

		return true;
	}

	void internal::heap_shutdown()
	{
		shutdown_region(s_retail);
		shutdown_region(s_development);
	}

	void internal::heap_map(RegionStats& retail, RegionStats& development, bool& overflowed)
	{
		read(s_retail, retail);
		read(s_development, development);
		overflowed = s_overflowed.load(std::memory_order_relaxed);
	}
}
