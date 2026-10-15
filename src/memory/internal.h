#pragma once

// Shared implementation details for the memory module. Keeping allocation
// headers and tracker hooks here lets the sources cooperate without exposing
// those details to clients.

#include <ember/core/macros.h>
#include <ember/memory/memory.h>
#include <ember/memory/tag.h>

#include <cstddef>

namespace ember::memory
{
	class Arena;
}

namespace ember::memory::internal
{
	// Initialize the heap over the supplied partitions, or discard its allocator
	// state. development may be null when its size is zero. Initialization returns
	// false if TLSF cannot be placed in a partition.
	bool heap_initialize(void* retail, size_t retail_size, void* development,
	    size_t development_size);
	void heap_shutdown();

	// Copy heap partition usage into the module's memory map.
	void heap_map(RegionStats& retail, RegionStats& development, bool& overflowed);

	// Stop after a memory lifetime violation, such as allocating before
	// initialize() or freeing after shutdown(). Continuing would touch storage
	// the module does not own.
	[[noreturn]] void fatal(const char* message);

	// Stored immediately before every heap payload. base_offset recovers the
	// raw allocation after alignment padding; size and tag let free() update
	// the right counters. magic helps catch a damaged header or invalid free,
	// but cannot prove that a pointer belongs to the heap.
	struct Header
	{
		u64 size;
		u32 base_offset;
		u8 tag;
		u8 reserved;
		u16 magic;
	};

	static_assert(sizeof(Header) == 16);

	inline constexpr u16 HEADER_MAGIC = 0xA11C;

#if EMBER_MEMORY_TRACKING >= 2
	// Extra metadata before Header at tracking level 2. The source location and
	// allocation number identify a block. The links keep it in the live list
	// until it is freed.
	struct DebugHeader
	{
		const char* file;
		Header* prev;
		Header* next;
		u32 line;
		u32 number;
	};

	static_assert(sizeof(DebugHeader) == 32);

	inline constexpr u64 TRAILING_GUARD = 0xDEADBEEFCAFEF00Dull;
#endif

	// Record a block after its header is initialized, and remove it before
	// invalidating that header. Level 0 uses empty inline hooks so tracking
	// adds no work on that path.
#if EMBER_MEMORY_TRACKING >= 1
	void track_allocate(Header* header);
	void track_free(Header* header);
#else
	inline void track_allocate(Header*)
	{
	}

	inline void track_free(Header*)
	{
	}
#endif

	// Register arenas for per-tag reporting between initialize() and shutdown().
	// Tracking level 0 needs no registry, so these hooks do nothing there.
#if EMBER_MEMORY_TRACKING >= 1
	void register_arena(const Arena* arena);
	void unregister_arena(const Arena* arena);
#else
	inline void register_arena(const Arena*)
	{
	}

	inline void unregister_arena(const Arena*)
	{
	}
#endif
}
