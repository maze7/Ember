#pragma once

#include "hash.h"
#include "core/common.h"

namespace Ember
{
	template<class T>
	struct Handle
	{
		u32 slot;
		u32 gen;

		inline static Handle<T> null = { .slot = 0, .gen = 0 };

		[[nodiscard]] constexpr bool is_valid() const { return *this != null; }

		[[nodiscard]] constexpr bool is_null() const { return *this == null; }

		[[nodiscard]] constexpr bool operator==(const Handle<T>& other) const {
				return slot == other.slot && gen == other.gen;
		}
	};
}

namespace std
{
	template <class T>
	struct hash<Ember::Handle<T>>
	{
		std::size_t operator()(const Ember::Handle<T>& handle) const {
			return Ember::combined_hash(handle.slot, handle.gen);
		}
	};
}