#pragma once

#include <ember/core/macros.h>

namespace ember
{
	template <typename T>
	EMBER_FINLINE constexpr T min(T a, T b)
	{
		return b < a ? b : a;
	}

	template <typename T>
	EMBER_FINLINE constexpr T max(T a, T b)
	{
		return a < b ? b : a;
	}

	// Limits [value] to [low, high]. [low] must not exceed [high].
	template <typename T>
	EMBER_FINLINE constexpr T clamp(T value, T low, T high)
	{
		EMBER_ASSERT(!(high < low));
		return value < low ? low : (high < value ? high : value);
	}
}
