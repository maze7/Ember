#pragma once

// Typed pool identifiers. The issuing pool checks the index and generation before resolving one.
// Handles neither retain objects nor identify their pool, so use them only with the issuing pool.

#include "ember/core/macros.h"
#include "ember/core/types.h"

namespace ember
{
	// Explicit packing fixes the bit layout independently of compiler bit-field rules.
	// Generations can repeat after 4095 reuses, allowing an old handle to match a later occupant.
	template <typename Tag>
	struct Handle
	{
		using tag_type = Tag;

		static constexpr u32 INDEX_BITS = 20;
		static constexpr u32 GENERATION_BITS = 12;
		static constexpr u32 MAX_INDEX = (1u << INDEX_BITS) - 1;
		static constexpr u32 MAX_GENERATION = (1u << GENERATION_BITS) - 1;

		// Zero is null. Restoring raw bits does not validate the referenced object.
		u32 bits = 0;

		// Fields must fit and generation must be nonzero, checked by assertions.
		[[nodiscard]] static constexpr Handle make(u32 index, u32 generation) noexcept
		{
			EMBER_ASSERT(index <= MAX_INDEX);
			EMBER_ASSERT(generation != 0 && generation <= MAX_GENERATION);

			return Handle{(generation << INDEX_BITS) | index};
		}

		[[nodiscard]] constexpr u32 index() const noexcept
		{
			return bits & MAX_INDEX;
		}

		[[nodiscard]] constexpr u32 generation() const noexcept
		{
			return bits >> INDEX_BITS;
		}

		[[nodiscard]] constexpr bool is_null() const noexcept
		{
			return bits == 0;
		}

		constexpr bool operator==(const Handle&) const noexcept = default;

		// Tests for a non-null handle. A non-null handle can still be stale.
		constexpr explicit operator bool() const noexcept
		{
			return !is_null();
		}
	};
}
