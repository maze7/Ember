#pragma once

// The engine's hash containers. They are ankerl::unordered_dense rather than the standard ones
// because it stores entries contiguously, which makes both iteration and lookup considerably
// faster than the node-based std::unordered_map. Both carry a TagAllocator by value, so
// HashMap<K, V> m(Tag::Assets) is charged to Assets wherever it goes. It is also the most
// expensive header in core to include, which is why this file is included only where a map is
// used and never from types.h.

#include <ember/containers/string.h>
#include <ember/core/handle.h>
#include <ember/core/types.h>
#include <ember/memory/allocator.h>

#include <ankerl/unordered_dense.h>

#include <functional>
#include <utility>

namespace ember
{
	template <typename K, typename V, typename Hash = ankerl::unordered_dense::hash<K>>
	using HashMap = ankerl::unordered_dense::map<K, V, Hash, std::equal_to<K>,
	    memory::TagAllocator<std::pair<K, V>>>;

	template <typename K, typename Hash = ankerl::unordered_dense::hash<K>>
	using HashSet =
	    ankerl::unordered_dense::set<K, Hash, std::equal_to<K>, memory::TagAllocator<K>>;
}

namespace ankerl::unordered_dense
{
	// Lets a String key a HashMap or HashSet. The library hashes std::string and string_view
	// but not a basic_string with another allocator, which an Ember String is; hashing it as
	// the view of its characters gives the same hash as the equivalent StringView, so a lookup
	// by view finds an entry keyed by String.
	template <>
	struct hash<ember::String>
	{
		using is_avalanching = void;

		[[nodiscard]] ember::u64 operator()(const ember::String& text) const noexcept
		{
			return hash<ember::StringView>{}(ember::StringView(text.data(), text.size()));
		}
	};

	// Lets a Handle key a HashMap or HashSet. A handle is its packed integer, and the map already
	// hashes a 32-bit integer well, so this delegates rather than mixing bits by hand.
	// is_avalanching tells the map the result is already well distributed, so it skips the extra
	// mixing it applies to hashes it does not trust. It lives here rather than in handle.h so that
	// handle.h, which everything includes, does not have to include a hash library.
	template <typename Tag>
	struct hash<ember::Handle<Tag>>
	{
		using is_avalanching = void;

		[[nodiscard]] ember::u64 operator()(ember::Handle<Tag> handle) const noexcept
		{
			return hash<ember::u32>{}(handle.bits);
		}
	};
}
