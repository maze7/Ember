#pragma once

// Alignment helpers shared by allocators, pools, and GPU resources.

#include <ember/core/macros.h>
#include <ember/core/types.h>

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ember
{
	// Raw allocations default to 16-byte alignment. Typed allocations request
	// the alignment of their element type.
	inline constexpr size_t DEFAULT_ALIGNMENT = 16;

	// Rounds value down to a multiple of alignment, which must be a power of two.
	template <std::unsigned_integral T>
	EMBER_FINLINE constexpr T align_down(T value, T alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));
		return value & ~(alignment - 1);
	}

	// Rounds value up to a power-of-two alignment. The addition must not overflow:
	// wrapping could turn an invalid offset into a plausible small one.
	template <std::unsigned_integral T>
	EMBER_FINLINE constexpr T align_up(T value, T alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));
		EMBER_ASSERT(value <= std::numeric_limits<T>::max() - (alignment - 1));
		return (value + (alignment - 1)) & ~(alignment - 1);
	}

	// The pointer overloads apply the same arithmetic through uintptr_t.
	// Their pointer-to-integer casts prevent use in constant expressions.
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

	// Tests whether pointer meets a power-of-two alignment. Assert the alignment
	// first so a bad request is not mistaken for an unaligned pointer.
	EMBER_FINLINE bool is_aligned(const void* pointer, size_t alignment)
	{
		EMBER_ASSERT(std::has_single_bit(alignment));
		return (reinterpret_cast<uintptr_t>(pointer) & (alignment - 1)) == 0;
	}
}
