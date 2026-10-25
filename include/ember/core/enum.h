#pragma once

// Enum names and opt-in flag operators. Register names with EMBER_ENUM_NAMES in the enum's
// namespace so argument-dependent lookup can find them.

#include <cstddef>
#include <type_traits> // IWYU pragma: keep

namespace ember
{
	template <typename T>
	struct EnumTag
	{
	};

	// Names follow the enum's declaration order and need no dynamic storage.
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

	template <typename T>
	concept NamedEnum = requires { ember_enum_names(EnumTag<T>{}); };

	template <typename T>
	constexpr auto enum_names()
	{
		return ember_enum_names(EnumTag<T>{});
	}

	// Values must be zero-based and contiguous, and value must be in the named range.
	template <class E>
	const char* enum_name(E value)
	{
		constexpr auto names = enum_names<E>();
		return names[static_cast<size_t>(value)];
	}

	// Check completeness when the enum supplies a Count sentinel.
	template <typename Type, size_t Count>
	constexpr void check_enum_count()
	{
		if constexpr (requires { Type::Count; })
		{
			static_assert(Count == static_cast<size_t>(Type::Count),
			    "EMBER_ENUM_NAMES: name count mismatch!");
		}
	}

// Place this in the enum's namespace. Supply one name per zero-based, contiguous value.
#define EMBER_ENUM_NAMES(Type, ...)                                                                \
	constexpr auto ember_enum_names(::ember::EnumTag<Type>)                                        \
	{                                                                                              \
		constexpr const char* raw[] = {__VA_ARGS__};                                               \
		constexpr auto names = ::ember::to_names(raw);                                             \
		::ember::check_enum_count<Type, names.size()>();                                           \
		return names;                                                                              \
	}
}

// Define flag operators in the enum's namespace so argument-dependent lookup finds them.
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
