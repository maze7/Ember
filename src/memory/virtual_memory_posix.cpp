#if !defined(_WIN32)

	#include <ember/core/macros.h>
	#include <ember/memory/alignment.h>
	#include <ember/memory/virtual_memory.h>

	#include <sys/mman.h>
	#include <unistd.h>

namespace ember::virtual_memory
{
	namespace
	{
		// These ranges come from our allocators, so invalid alignment or size is
		// a caller bug. Keep those checks separate from operating-system failures.
		void assert_page_range(const void* address, size_t size)
		{
			EMBER_ASSERT(address != nullptr);
			EMBER_ASSERT(is_aligned(address, page_size()));
			EMBER_ASSERT(size != 0 && size % page_size() == 0);
		}
	}

	size_t page_size()
	{
		static const size_t s_page_size = (size_t)sysconf(_SC_PAGESIZE);
		return s_page_size;
	}

	size_t allocation_granularity()
	{
		// mmap already aligns reservations to pages, so no larger granularity is needed.
		return page_size();
	}

	void* reserve(size_t size)
	{
		EMBER_ASSERT(size != 0);

		const size_t rounded = align_up(size, allocation_granularity());
		void* address = mmap(nullptr, rounded, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		return address == MAP_FAILED ? nullptr : address;
	}

	bool commit(void* address, size_t size)
	{
		assert_page_range(address, size);
		return mprotect(address, size, PROT_READ | PROT_WRITE) == 0;
	}

	bool decommit(void* address, size_t size)
	{
		assert_page_range(address, size);

	// Discarding backing pages does not remove access to the addresses.
	// Protect the range as well so stale pointers fault until it is committed again.
	// macOS uses MADV_FREE here; the other POSIX path uses MADV_DONTNEED.
	#if defined(__APPLE__)
		const bool advised = madvise(address, size, MADV_FREE) == 0;
	#else
		const bool advised = madvise(address, size, MADV_DONTNEED) == 0;
	#endif

		return mprotect(address, size, PROT_NONE) == 0 && advised;
	}

	bool release(void* address, size_t size)
	{
		assert_page_range(address, size);
		return munmap(address, size) == 0;
	}
}

#endif
