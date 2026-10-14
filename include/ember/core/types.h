#pragma once

// This header contains the type aliases used across the entire Ember codebase. Every other Ember
// header includes it, so it must stay small and must not pull in anything heavy.
#include <cstdint>

namespace Ember
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
}
