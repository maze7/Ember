#pragma once

#include "core/common.h"

namespace Ember::Math
{
	static constexpr double EPSILON = 0.000001;

	template<class T>
	constexpr T abs(T x) {
		return x < 0 ? -x : x;
	}

	template<class T>
	constexpr T sign(T x) {
		return static_cast<T>(x == 0 ? 0 : (x < 0 ? -1 : 1));
	}

	template<class T, class TMin, class TMax>
	constexpr T clamp(T value, TMin min, TMax max) {
		return value < min ? static_cast<T>(min) : (value > max ? static_cast<T>(max) : value);
	}

	template<class T>
	constexpr T min(T a, T b) {
		return  (T)(a < b ? a : b);
	}

	template<class T, typename ... Args>
	constexpr T min(const T& a, const T& b, const Args&... args) {
		return Math::min(a, Math::min(b, args...));
	}

	template<class T>
	constexpr T max(T a, T b) {
		return (T)(a > b ? a : b);
	}

	template<class T, typename ... Args>
	constexpr T max(const T& a, const T& b, const Args&... args) {
		return Math::max(a, Math::max(b, args...));
	}

	constexpr f32 approach(f32 t, f32 target, f32 delta) {
		return t < target ? Math::min(t + delta, target) : Math::max(t - delta, target);
	}

	constexpr f32 map(f32 t, f32 old_min, f32 old_max, f32 new_min, f32 new_max) {
		return new_min + ((t - old_min) / (old_max - old_min)) * (new_max - new_min);
	}

	constexpr f32 clamped_map(f32 t, const f32 old_min, f32 old_max, f32 new_min, f32 new_max) {
		return map(Math::clamp(t, old_min, old_max), old_min, old_max, new_min, new_max);
	}

	constexpr f32 lerp(f32 a, f32 b, f32 t)
	{
		return a + (b - a) * t;
	}
}