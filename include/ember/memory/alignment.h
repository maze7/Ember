#pragma once

// Alignment arithmetic for the memory module, pools and the GPU backend. Everything else of this
// kind that Ember needs is in <bit> as of C++20 and is used from there directly. This file holds
// only what the standard library does not offer.

#include <ember/core/macros.h>
#include <ember/core/types.h>

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ember
{
	// What an allocator hands out when the caller does not say. It fits every scalar and a 128-bit
	// SIMD lane, and it is what malloc guarantees on the platforms Ember runs on.
	inline constexpr size_t DEFAULT_ALIGNMENT = 16;

	// Rounds [value] down to a multiple of [alignment], which must be a power of two.
	template <std::unsigned_integral T>
	EMBER_FINLINE constexpr T align_down(T value, T alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));
		return value & ~(alignment - 1);
	}

	// Rounds [value] up to a multiple of [alignment]. The result must fit in T: a value within one
	// alignment of the type's maximum would wrap to a small number and pass every later check, so
	// that is asserted too.
	template <std::unsigned_integral T>
	EMBER_FINLINE constexpr T align_up(T value, T alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));
		EMBER_ASSERT(value <= std::numeric_limits<T>::max() - (alignment - 1));
		return (value + (alignment - 1)) & ~(alignment - 1);
	}

	// The same two for addresses. These are not constexpr, because reinterpret_cast is not allowed
	// in constant evaluation and an address is never a compile-time quantity anyway.
	template <typename T>
	EMBER_FINLINE T* align_down(T* pointer, size_t alignment)
	{
		return reinterpret_cast<T*>(align_down(reinterpret_cast<uintptr_t>(pointer), alignment));
	}

	template <typename T>
	EMBER_FINLINE T* align_up(T* pointer, size_t alignment)
	{
		return reinterpret_cast<T*>(align_up(reinterpret_cast<uintptr_t>(pointer), alignment));
	}

	// True when [pointer] is a multiple of [alignment], which must be a power of two. A bad
	// alignment asserts here like everywhere else, rather than reporting the pointer as unaligned
	// and letting the real mistake hide behind a plausible answer.
	EMBER_FINLINE bool is_aligned(const void* pointer, size_t alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));
		return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
	}
}
