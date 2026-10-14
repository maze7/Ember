#pragma once

// A typed, non-owning identifier for an entry in a pool. The index selects a slot, and the
// generation distinguishes successive occupants of that slot.
//
// A pool must check that the slot is live and its generation matches before resolving a handle.
// Holding a handle does not keep the object alive.
//
// Tags distinguish handle types at compile time without adding runtime storage. Handles carry no
// pool identity, so the caller must resolve them against the pool that issued them.

#include "ember/core/macros.h"
#include "ember/core/types.h"

namespace ember
{
	// One shape for every handle in the engine: a u32 holding a 20-bit index and a 12-bit
	// generation. Twenty bits address 1,048,575 slots, twelve bits means a slot's generation wraps
	// after 4095 reuses. This is an intentional trade.
	//
	// The halves are packed by hand rather than declared as bit-fields because a bit-field's layout
	// is defined by the compiler, and these bits are intended to be serialized.
	template <typename Tag>
	struct Handle
	{
		using tag_type = Tag;

		static constexpr u32 INDEX_BITS = 20;
		static constexpr u32 GENERATION_BITS = 12;
		static constexpr u32 MAX_INDEX = (1u << INDEX_BITS) - 1;
		static constexpr u32 MAX_GENERATION = (1u << GENERATION_BITS) - 1;

		// The low bits hold the index; the high bits hold the generation. Handle{bits} restores
		// these fields without checking whether a pool issued them or whether the referenced object
		// still exists.
		u32 bits = 0;

		// Packs the fields with validation in debug builds. In release builds both arguments must
		// fit their bit ranges; excess bits are discarded.
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

		// A pool must use different generations for successive occupants of a slot. Generations are
		// finite: repeating a value can make a stale handle match a later occupant of the same
		// slot.
		[[nodiscard]] constexpr u32 generation() const noexcept
		{
			return bits >> INDEX_BITS;
		}

		// Null is zero, the way a null pointer is, so that a value-initialized handle means "none".
		[[nodiscard]] constexpr bool is_null() const noexcept
		{
			return bits == 0;
		}

		constexpr bool operator==(const Handle&) const noexcept = default;
	};
}
