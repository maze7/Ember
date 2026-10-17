#pragma once

// A value and the error that came with it, for calls that can fail. Results are used as an
// alternative to exceptions, as the engine is designed to be exception free. Ember follows the
// "Zero is success" ideaology. It reads like so:
//
// 		if (auto opened = fs::open(path, mode); !opened)
// 		{
// 			return opened.error;
// 		}
//
// 		use(*opened);
//
// 		auto [file, error] = fs::open(path, mode);
//
// A function with nothing but an error to report returns Result<void, E>.

#include <ember/core/macros.h>
#include <ember/core/types.h>

#include <type_traits>

namespace ember
{
	template <typename T, typename E>
	struct [[nodiscard]] Result
	{
		static_assert(std::is_enum_v<E> && static_cast<int>(E::None) == 0,
		    "an error enum names its zero None");
		static_assert(!std::is_same_v<T, E>, "a value that is its own error would be ambiguous");
		static_assert(std::is_trivially_copyable_v<T>, "a result carries plain data");

		T value = {};
		E error = E::None;

		Result() = default;

		Result(T success) : value(success)
		{
		}

		Result(E failed) : error(failed)
		{
		}

		EMBER_FINLINE explicit operator bool() const
		{
			return error == E::None;
		}

		EMBER_FINLINE T& operator*()
		{
			return value;
		}

		EMBER_FINLINE const T& operator*() const
		{
			return value;
		}

		EMBER_FINLINE T* operator->()
		{
			return &value;
		}

		EMBER_FINLINE const T* operator->() const
		{
			return &value;
		}

		// The value, or [fallback] on a failure.
		EMBER_FINLINE T value_or(T fallback) const
		{
			return error == E::None ? value : fallback;
		}
	};

	template <typename E>
	struct [[nodiscard]] Result<void, E>
	{
		static_assert(std::is_enum_v<E> && static_cast<int>(E::None) == 0,
		    "an error enum names its zero None");

		E error = E::None;

		Result() = default;

		Result(E failed) : error(failed)
		{
		}

		EMBER_FINLINE explicit operator bool() const
		{
			return error == E::None;
		}
	};
}
