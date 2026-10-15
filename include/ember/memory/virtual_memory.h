#pragma once

// Reserves address space and controls which pages allocators can access.
// Reservation keeps the addresses available; commitment makes them usable.
//
// commit(), decommit(), and release() require a nonempty, page-aligned range.
// release() must receive the whole reservation. These are caller contracts,
// checked with assertions; operating-system failures are returned separately.

#include <ember/core/types.h>

#include <cstddef>

namespace ember::virtual_memory
{
	// The page size used to align committed and protected ranges.
	size_t page_size();

	// Alignment required for reservations, which may be larger than a page.
	size_t allocation_granularity();

	// Reserves inaccessible address space, rounding size up to the reservation
	// granularity. Returns null if the operating system rejects the request.
	[[nodiscard]] void* reserve(size_t size);

	// Makes the range readable and writable. Returns false if the operating
	// system rejects the request.
	[[nodiscard]] bool commit(void* address, size_t size);

	// Makes the range inaccessible and asks the operating system to discard its
	// backing pages while retaining the addresses. Treat the contents as discarded;
	// do not access the range until it has been committed again.
	[[nodiscard]] bool decommit(void* address, size_t size);

	// Releases the whole reservation. Pass the rounded size used by reserve().
	[[nodiscard]] bool release(void* address, size_t size);
}
