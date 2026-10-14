#pragma once

// An enum opts in to having its values named by placing EMBER_ENUM_NAMES in scope. The machinery
// works by argument-dependent lookup on EnumTag<T>: the macro defines an overload of
// ember_enum_names taking that tag, and the helpers below find it from wherever the macro was
// placed.

#include <cstddef>

namespace Ember
{
	// Empty tag type used only to select the right ember_enum_names overload.
	template <typename T>
	struct EnumTag
	{
	};

	/// Satisfied when EMBER_ENUM_NAMES(T, ...) is in scope for T.
	template <typename T>
	concept NamedEnum = requires { ember_enum_names(EnumTag<T>{}); };

	// Returns the array of names registered for T, in declaration order.
	template <typename T>
	constexpr auto enum_names()
	{
		return ember_enum_names(EnumTag<T>{});
	}

	// Returns the name registered for a single enum value. The value is not range checked: passing
	// one outside the named range is a bug in the caller.
	template <class E>
	const char* enum_name(E value)
	{
		constexpr auto names = enum_names<E>();
		return names[static_cast<size_t>(value)];
	}

	// Fails to compile when the number of names passed to EMBER_ENUM_NAMES does not match the
	// enum's Count member. Enums without a Count member are not checked, since there is nothing to
	// check against.
	template <typename Type, size_t Count>
	constexpr void check_enum_count()
	{
		if constexpr (requires { Type::Count; })
		{
			static_assert(Count == static_cast<size_t>(Type::Count),
			    "EMBER_ENUM_NAMES: name count mismatch!");
		}
	}
}

// Names an enum's values for enum_name and for JSON serialization. Place it in the Ember namespace,
// at global scope, or in the enum's own namespace: ADL on EnumTag finds it from any of those.
#define EMBER_ENUM_NAMES(Type, ...)                                                                \
	constexpr auto ember_enum_names(::Ember::EnumTag<Type>) noexcept                               \
	{                                                                                              \
		constexpr const char* raw[] = {__VA_ARGS__};                                               \
		constexpr auto arr = std::to_array(raw);                                                   \
		::Ember::check_enum_count<Type, arr.size()>();                                             \
		return arr;                                                                                \
	}
