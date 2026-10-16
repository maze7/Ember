#pragma once

// This header contains the type aliases used across the entire Ember codebase. Every other Ember
// header includes it, so it must stay small and must not pull in anything heavy.
#include <cstdint>

namespace ember
{
	// Ember uses fixed-width integer types everywhere so that struct layouts and serialized data
	// mean the same thing on every platform. The names are short because they are typed constantly.
	using u8 = uint8_t;
	using u16 = uint16_t;
	using u32 = uint32_t;
	using u64 = uint64_t;

	using i8 = int8_t;
	using i16 = int16_t;
	using i32 = int32_t;
	using i64 = int64_t;

	using f32 = float;
	using f64 = double;

	// Byte sizes read better as units than as products of 1024. They are consteval so a size can
	// only be spelled with a literal, which is the only place a unit belongs.
	consteval u64 operator""_kb(unsigned long long value) noexcept
	{
		return static_cast<u64>(value) * 1024;
	}

	consteval u64 operator""_mb(unsigned long long value) noexcept
	{
		return static_cast<u64>(value) * 1024 * 1024;
	}

	consteval u64 operator""_gb(unsigned long long value) noexcept
	{
		return static_cast<u64>(value) * 1024 * 1024 * 1024;
	}
}
