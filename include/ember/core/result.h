#pragma once

#include <tl/expected.hpp>
#include <type_traits>
#include <utility>

namespace ember
{
	template <class T, class E>
	using Result = tl::expected<T, E>;

	template <class E>
	[[nodiscard]] constexpr auto fail(E&& error)
	{
		using Error = std::decay_t<E>;
		return tl::unexpected<Error>(std::forward<E>(error));
	}
}
