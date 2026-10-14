#include <ember/core/enum.h>
#include <ember/memory/heap.h>
#include <ember/memory/memory.h>
#include <ember/memory/tracker.h>

#include <bit>
#include <cstring>

// rpmalloc is the default. The system allocator is for sanitizer builds, which can only see
// through malloc, and for a first port to a platform rpmalloc does not know yet.
#ifndef EMBER_USE_RPMALLOC
	#define EMBER_USE_RPMALLOC 1
#endif

#if EMBER_USE_RPMALLOC
	#include <rpmalloc.h>
#elif defined(_WIN32)
	#include <malloc.h>
#elif defined(__APPLE__)
	#include <cstdlib>
	#include <malloc/malloc.h>
#else
	#include <cstdlib>
	#include <malloc.h>
#endif

namespace ember
{
	namespace
	{
		void* raw_allocate(size_t size, size_t alignment, bool zeroed)
		{
#if EMBER_USE_RPMALLOC
			if (alignment <= DEFAULT_ALIGNMENT)
			{
				return zeroed ? rpzalloc(size) : rpmalloc(size);
			}

			return zeroed ? rpaligned_zalloc(alignment, size) : rpaligned_alloc(alignment, size);
#elif defined(_WIN32)
			const size_t bytes = size == 0 ? 1 : size;
			void* ptr = _aligned_malloc(bytes,
			    alignment < DEFAULT_ALIGNMENT ? DEFAULT_ALIGNMENT : alignment);
			if (zeroed && ptr != nullptr)
			{
				std::memset(ptr, 0, bytes);
			}

			return ptr;
#else
			const size_t bytes = size == 0 ? 1 : size;
			if (alignment <= DEFAULT_ALIGNMENT)
			{
				return zeroed ? std::calloc(1, bytes) : std::malloc(bytes);
			}

			// aligned_alloc wants a size that is a multiple of the alignment.
			const size_t rounded = align_up(bytes, alignment);
			void* ptr = std::aligned_alloc(alignment, rounded);
			if (zeroed && ptr != nullptr)
			{
				std::memset(ptr, 0, rounded);
			}

			return ptr;
#endif
		}

		void raw_free(void* ptr)
		{
#if EMBER_USE_RPMALLOC
			rpfree(ptr);
#elif defined(_WIN32)
			_aligned_free(ptr);
#else
			std::free(ptr);
#endif
		}

		void* raw_reallocate(void* ptr, size_t size)
		{
#if EMBER_USE_RPMALLOC
			return rprealloc(ptr, size);
#elif defined(_WIN32)
			return _aligned_realloc(ptr, size == 0 ? 1 : size, DEFAULT_ALIGNMENT);
#else
			return std::realloc(ptr, size == 0 ? 1 : size);
#endif
		}

		size_t raw_usable_size(void* ptr)
		{
#if EMBER_USE_RPMALLOC
			return rpmalloc_usable_size(ptr);
#elif defined(_WIN32)
			return _aligned_msize(ptr, DEFAULT_ALIGNMENT, 0);
#elif defined(__APPLE__)
			return malloc_size(ptr);
#else
			return malloc_usable_size(ptr);
#endif
		}

		// One view per category, in declaration order. They are built on first use, so that a
		// static object in any translation unit may allocate during its own initialization, and
		// never destroyed, so that one may still free through a view after main() returns: an
		// ordinary static would be destroyed at a point of exit that depends on link order, and a
		// union member is only destroyed if the union's destructor does it, which this one's does
		// not. Constant initialization would have done the first half without the guard that a
		// function-local static costs, but a polymorphic object as a constant initializer is
		// something compilers disagree about, and heap() is not on an allocation path.
		union Views
		{
			Heap heaps[(size_t)Category::Count];

			~Views()
			{
			}
		};

		Views& views()
		{
			static Views s_views = {{
			    Heap{Category::Unknown},
			    Heap{Category::Engine},
			    Heap{Category::Graphics},
			    Heap{Category::Audio},
			    Heap{Category::Physics},
			    Heap{Category::Gameplay},
			    Heap{Category::Assets},
			    Heap{Category::Scripting},
			    Heap{Category::Network},
			    Heap{Category::Platform},
			    Heap{Category::Input},
			    Heap{Category::Tools},
			}};

			return s_views;
		}

		static_assert((size_t)Category::Count == 12,
		    "a new Category needs its Heap added to views()");
	}

	void* Heap::do_allocate(size_t bytes, size_t alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));

		void* ptr = raw_allocate(bytes, alignment, false);
		if (ptr == nullptr) [[unlikely]]
		{
			memory::out_of_memory(bytes, alignment, enum_name(category));
		}

#if EMBER_MEMORY_TRACKING >= 2
		// Fresh memory is filled so that a read before a write shows a pattern, not a plausible
		// value left by whatever was here before.
		std::memset(ptr, 0xCD, bytes);
#endif

		internal::track_allocate(ptr, usable_size(ptr), category);
		return ptr;
	}

	void Heap::do_deallocate(void* ptr, size_t, size_t)
	{
		// The allocator underneath frees without a size, so the sized and unsized paths are one.
		deallocate_unsized(ptr);
	}

	bool Heap::do_is_equal(const Allocator& other) const noexcept
	{
		return this == &other;
	}

	void* Heap::allocate_zeroed(size_t size, size_t alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));

		void* ptr = raw_allocate(size, alignment, true);
		if (ptr == nullptr) [[unlikely]]
		{
			memory::out_of_memory(size, alignment, enum_name(category));
		}

		internal::track_allocate(ptr, usable_size(ptr), category);
		return ptr;
	}

	void* Heap::reallocate(void* ptr, size_t new_size)
	{
		if (ptr != nullptr)
		{
			internal::track_free(ptr, usable_size(ptr), category);
		}

		void* new_ptr = raw_reallocate(ptr, new_size);
		if (new_ptr == nullptr) [[unlikely]]
		{
			memory::out_of_memory(new_size, DEFAULT_ALIGNMENT, enum_name(category));
		}

		internal::track_allocate(new_ptr, usable_size(new_ptr), category);
		return new_ptr;
	}

	void Heap::deallocate_unsized(void* ptr)
	{
		if (ptr == nullptr)
		{
			return;
		}

		const size_t size = usable_size(ptr);
		internal::track_free(ptr, size, category);

#if EMBER_MEMORY_TRACKING >= 2
		// Freed memory is filled too, so a stale pointer reads the pattern instead of the object
		// that used to be there.
		std::memset(ptr, 0xDD, size);
#endif

		raw_free(ptr);
	}

	size_t Heap::usable_size(void* ptr)
	{
		return ptr == nullptr ? 0 : raw_usable_size(ptr);
	}

	Heap* memory::heap(Category category)
	{
		const size_t index = (size_t)category;
		EMBER_ASSERT(index < (size_t)Category::Count);
		return &views().heaps[index < (size_t)Category::Count ? index : 0];
	}

	void internal::heap_initialize()
	{
#if EMBER_USE_RPMALLOC
		// A no-op when a static object already allocated and so already brought rpmalloc up.
		(void)rpmalloc_initialize(nullptr);
#endif
	}

	void internal::heap_initialize_thread()
	{
#if EMBER_USE_RPMALLOC
		if (!rpmalloc_is_thread_initialized())
		{
			rpmalloc_thread_initialize();
		}
#endif
	}

	void internal::heap_shutdown_thread()
	{
#if EMBER_USE_RPMALLOC
		if (rpmalloc_is_thread_initialized())
		{
			rpmalloc_thread_finalize();
		}
#endif
	}
}
