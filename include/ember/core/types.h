#pragma once

// Shared scalar types and byte-size literals. Keep this header's dependencies small.

#include <cstdint>

namespace ember
{
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

	// Binary byte units, evaluated at compile time.
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
