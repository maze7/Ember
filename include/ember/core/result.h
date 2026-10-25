#pragma once

// Plain values and explicit errors for boot and I/O boundaries. E::None must be zero.
// Check success before using the value; dereference operators do not check the error.
// Use Result<void, E> when a call has no value to return.

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
