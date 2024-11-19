#pragma once

namespace Ember
{
	template <class T>
	struct Vector2
	{
		T x, y;

		Vector2() = default;
		Vector2(T a, T b) : x(a), y(b) {}

		T& operator[](int i) { return ((&x)[i]); }
		const T& operator[](int i) const { return ((&x)[i]); }
	};

	using Vector2f = Vector2<float>;
	using Vector2d = Vector2<double>;
	using Vector2i = Vector2<int>;
	using Vector2u = Vector2<u32>;
}