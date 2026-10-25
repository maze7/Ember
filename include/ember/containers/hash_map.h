#pragma once

// Dense hash maps and sets with engine allocation tags. Keep this header at map call sites
// rather than in shared type headers because it includes the hash library.

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
	// Hash String through StringView so allocator choice does not change the character hash.
	template <>
	struct hash<ember::String>
	{
		using is_avalanching = void;

		[[nodiscard]] ember::u64 operator()(const ember::String& text) const noexcept
		{
			return hash<ember::StringView>{}(ember::StringView(text.data(), text.size()));
		}
	};

	// Reuse the library's integer hash and mark it mixed. Keeping this specialization here
	// avoids exposing the hash library through handle.h.
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
