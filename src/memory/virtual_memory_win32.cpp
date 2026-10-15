#if defined(_WIN32)

	#include <ember/core/macros.h>
	#include <ember/memory/alignment.h>
	#include <ember/memory/virtual_memory.h>

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>

namespace ember::virtual_memory
{
	namespace
	{
		// Cache both values from one system query on first use.
		struct PageInfo
		{
			size_t page_size = 0;
			size_t allocation_granularity = 0;
		};

		const PageInfo& page_info()
		{
			static const PageInfo s_info = []
			{
				SYSTEM_INFO info = {};
				GetSystemInfo(&info);
				return PageInfo{(size_t)info.dwPageSize, (size_t)info.dwAllocationGranularity};
			}();

			return s_info;
		}

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
		return page_info().page_size;
	}

	size_t allocation_granularity()
	{
		return page_info().allocation_granularity;
	}

	void* reserve(size_t size)
	{
		EMBER_ASSERT(size != 0);

		const size_t rounded = align_up(size, allocation_granularity());
		return VirtualAlloc(nullptr, rounded, MEM_RESERVE, PAGE_NOACCESS);
	}

	bool commit(void* address, size_t size)
	{
		assert_page_range(address, size);
		return VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
	}

	bool decommit(void* address, size_t size)
	{
		assert_page_range(address, size);
		return VirtualFree(address, size, MEM_DECOMMIT) != 0;
	}

	bool release(void* address, size_t size)
	{
		assert_page_range(address, size);

		// MEM_RELEASE requires the reservation's base address and a zero size.
		// We still check the caller's size above for consistency with the other APIs.
		return VirtualFree(address, 0, MEM_RELEASE) != 0;
	}
}

#endif
