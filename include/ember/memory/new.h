#pragma once

// Constructs objects through an Allocator. Deletion must use the same allocator
// and the original object type so the size and alignment match.

#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <new>
#include <source_location>
#include <type_traits>
#include <utility>

namespace ember::memory
{
	// Construction must not throw. Allocation failure stops the process.
	//
	// Detailed leak reports identify these allocations by tag and allocation
	// number; this helper passes no source location.
	template <typename T, typename... Args>
	[[nodiscard]] T* new_object(Allocator* allocator, Tag tag, Args&&... args) noexcept
	{
		void* memory = allocator->allocate(tag, sizeof(T), alignof(T), std::source_location{});
		return ::new (memory) T(std::forward<Args>(args)...);
	}

	// Destroys object and returns its storage to the allocator that created it.
	// A null pointer is harmless. T must be the original allocation type because
	// deallocation uses sizeof(T) and alignof(T).
	template <typename T>
	void delete_object(Allocator* allocator, T* object) noexcept
	{
		static_assert(std::is_nothrow_destructible_v<T>, "destructors must not throw");

		if (object == nullptr)
		{
			return;
		}

		object->~T();
		allocator->deallocate(object, sizeof(T), alignof(T));
	}
}
