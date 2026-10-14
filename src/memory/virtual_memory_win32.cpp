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
		// Both sizes come from one query, made on first use.
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

		// The checks every call makes: a page aligned address and a non-empty, page sized range.
		// All of it is caller contract, so it only asserts.
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

		// MEM_RELEASE takes the base address and a size of zero; the size is only checked here.
		return VirtualFree(address, 0, MEM_RELEASE) != 0;
	}
}

#endif
