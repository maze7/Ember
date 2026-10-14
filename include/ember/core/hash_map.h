#pragma once

// The engine's hash containers. They are ankerl::unordered_dense rather than the standard ones
// because it stores entries contiguously, which makes both iteration and lookup considerably faster
// than the node-based std::unordered_map. It is also the most expensive header in core to include,
// which is why this file is included only where a map is used and never from types.h.

#include <ember/core/handle.h>
#include <ember/core/types.h>

#include <ankerl/unordered_dense.h>

namespace Ember
{
	template <typename K, typename V, typename Hash = ankerl::unordered_dense::hash<K>>
	using HashMap = ankerl::unordered_dense::pmr::map<K, V, Hash>;

	template <typename K, typename Hash = ankerl::unordered_dense::hash<K>>
	using HashSet = ankerl::unordered_dense::pmr::set<K, Hash>;
}

namespace ankerl::unordered_dense
{
	// Lets a Handle key a HashMap or HashSet. A handle is its packed integer, and the map already
	// hashes a 32-bit integer well, so this delegates rather than mixing bits by hand.
	// is_avalanching tells the map the result is already well distributed, so it skips the extra
	// mixing it applies to hashes it does not trust. It lives here rather than in handle.h so that
	// handle.h, which everything includes, does not have to include a hash library.
	template <typename Tag>
	struct hash<Ember::Handle<Tag>>
	{
		using is_avalanching = void;

		[[nodiscard]] Ember::u64 operator()(Ember::Handle<Tag> handle) const noexcept
		{
			return hash<Ember::u32>{}(handle.bits);
		}
	};
}
