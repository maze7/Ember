#pragma once

// The platform's pages, as Ember's allocators see them. Address space is reserved first and
// committed later, in the Win32 shape; on POSIX a reservation is a PROT_NONE mapping and a commit
// is the mprotect that makes it usable. Physical pages arrive on first touch either way.
//
// Every address and size handed to commit, decommit and release must be page aligned, and a
// release must name the whole reservation. The callers are the allocators in this module, which
// is why those are assertions rather than error returns.

#include <ember/core/types.h>

#include <cstddef>

namespace ember::virtual_memory
{
	// The unit the kernel maps in, and the alignment of every address returned by reserve().
	size_t page_size();

	// The unit reservations are made in: 64 KiB on Windows, a page everywhere else.
	size_t allocation_granularity();

	// Claims address space with no access, rounded up to the allocation granularity. Null when
	// the system refuses.
	[[nodiscard]] void* reserve(size_t size);

	// Makes pages readable and writable. False when the system is out of memory.
	[[nodiscard]] bool commit(void* address, size_t size);

	// Gives pages back while keeping the address space. Their contents are gone, and touching
	// them before the next commit faults.
	[[nodiscard]] bool decommit(void* address, size_t size);

	// Returns a reservation to the system. The size is the rounded size reserve() applied.
	[[nodiscard]] bool release(void* address, size_t size);
}
