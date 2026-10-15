#pragma once

// Object construction lives here so callers that only need new_object or
// Unique do not have to include the rest of the memory module.

#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <new>
#include <source_location>
#include <type_traits>
#include <utility>

namespace ember::memory
{
	// Allocates and constructs a T. Construction must not throw. Allocation
	// failure stops the process, so a returned pointer refers to a live object.
	//
	// Destroy it with delete_object through the same allocator. This helper
	// passes an empty source location, so detailed leak reports identify the
	// allocation by its tag and allocation number.
	template <typename T, typename... Args>
	[[nodiscard]] T* new_object(Allocator* allocator, Tag tag, Args&&... args)
	{
		static_assert(std::is_nothrow_constructible_v<T, Args&&...>,
		    "construction must not throw; fallible setup belongs in a create() function");

		void* memory = allocator->allocate(tag, sizeof(T), alignof(T), std::source_location{});
		return ::new (memory) T(std::forward<Args>(args)...);
	}

	// Destroys object and returns its storage to the allocator that created it.
	// A null pointer is harmless. T must be the original allocation type because
	// deallocation uses sizeof(T) and alignof(T).
	template <typename T>
	void delete_object(Allocator* allocator, T* object)
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
