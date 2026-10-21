#pragma once

// An enum opts in to having its values named by placing EMBER_ENUM_NAMES in scope. The machinery
// works by argument-dependent lookup on EnumTag<T>: the macro defines an overload of
// ember_enum_names taking that tag, and the helpers below find it from wherever the macro was
// placed.

#include <cstddef>
#include <type_traits> // IWYU pragma: keep

namespace ember
{
	// Empty tag type used only to select the right ember_enum_names overload.
	template <typename T>
	struct EnumTag
	{
	};

	// The names of an enum's values, in declaration order: a fixed array that knows its size and
	// can be walked, and nothing more, so that a header naming an enum stays cheap.
	template <size_t N>
	struct EnumNames
	{
		const char* names[N];

		constexpr size_t size() const
		{
			return N;
		}

		constexpr const char* operator[](size_t index) const
		{
			return names[index];
		}

		constexpr const char* const* begin() const
		{
			return names;
		}

		constexpr const char* const* end() const
		{
			return names + N;
		}
	};

	// Copies a braced list of names into an EnumNames, deducing the count from the array.
	template <size_t N>
	constexpr EnumNames<N> to_names(const char* const (&raw)[N])
	{
		EnumNames<N> names = {};
		for (size_t i = 0; i < N; ++i)
		{
			names.names[i] = raw[i];
		}
		return names;
	}

	/// Satisfied when EMBER_ENUM_NAMES(T, ...) is in scope for T.
	template <typename T>
	concept NamedEnum = requires { ember_enum_names(EnumTag<T>{}); };

	// Returns the names registered for T, in declaration order.
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

// Names an enum's values for enum_name and for JSON serialization. Place it in the Ember namespace,
// at global scope, or in the enum's own namespace: ADL on EnumTag finds it from any of those.
#define EMBER_ENUM_NAMES(Type, ...)                                                                \
	constexpr auto ember_enum_names(::ember::EnumTag<Type>)                                        \
	{                                                                                              \
		constexpr const char* raw[] = {__VA_ARGS__};                                               \
		constexpr auto names = ::ember::to_names(raw);                                             \
		::ember::check_enum_count<Type, names.size()>();                                           \
		return names;                                                                              \
	}
}

// Bitwise operator for an enum class that names a set of flags. A scoped enum comes with no
// operators at all, which is right for an enum that names one of several things and wrong for one
// that names any combination of them. This macro opts an enum into the second kind. Call it in the
// enum's own namespace, right after the enum, so that argument-dependent lookup finds the operators
// wherever a value of the enum is used.
#define EMBER_ENUM_BITWISE_OPS(EnumType)                                                           \
	static_assert(std::is_enum_v<EnumType>, "EMBER_ENUM_BITWISE_OPS needs an enum");               \
                                                                                                   \
	[[nodiscard]] constexpr EnumType operator|(EnumType lhs, EnumType rhs)                         \
	{                                                                                              \
		using U = std::underlying_type_t<EnumType>;                                                \
		return static_cast<EnumType>(static_cast<U>(lhs) | static_cast<U>(rhs));                   \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr EnumType operator&(EnumType lhs, EnumType rhs)                         \
	{                                                                                              \
		using U = std::underlying_type_t<EnumType>;                                                \
		return static_cast<EnumType>(static_cast<U>(lhs) & static_cast<U>(rhs));                   \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr EnumType operator^(EnumType lhs, EnumType rhs)                         \
	{                                                                                              \
		using U = std::underlying_type_t<EnumType>;                                                \
		return static_cast<EnumType>(static_cast<U>(lhs) ^ static_cast<U>(rhs));                   \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr EnumType operator~(EnumType value)                                     \
	{                                                                                              \
		using U = std::underlying_type_t<EnumType>;                                                \
		return static_cast<EnumType>(~static_cast<U>(value));                                      \
	}                                                                                              \
                                                                                                   \
	constexpr EnumType& operator|=(EnumType& lhs, EnumType rhs)                                    \
	{                                                                                              \
		return lhs = lhs | rhs;                                                                    \
	}                                                                                              \
                                                                                                   \
	constexpr EnumType& operator&=(EnumType& lhs, EnumType rhs)                                    \
	{                                                                                              \
		return lhs = lhs & rhs;                                                                    \
	}                                                                                              \
                                                                                                   \
	constexpr EnumType& operator^=(EnumType& lhs, EnumType rhs)                                    \
	{                                                                                              \
		return lhs = lhs ^ rhs;                                                                    \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr bool any(EnumType value)                                               \
	{                                                                                              \
		return static_cast<std::underlying_type_t<EnumType>>(value) != 0;                          \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr bool none(EnumType value)                                              \
	{                                                                                              \
		return !any(value);                                                                        \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr bool has_any(EnumType value, EnumType flags)                           \
	{                                                                                              \
		return any(value & flags);                                                                 \
	}                                                                                              \
                                                                                                   \
	[[nodiscard]] constexpr bool has_all(EnumType value, EnumType flags)                           \
	{                                                                                              \
		return (value & flags) == flags;                                                           \
	}
