#pragma once

#include <type_traits>
#include "vector2.h"

namespace Ember
{
	template <class T> requires std::is_arithmetic_v<T>
	struct Rect
	{
		Rect() : x(0), y(0), width(0), height(0) {}

		Rect(T x, T y, T width, T height)
			: x(x), y(y), width(width), height(height) {}

		Rect(Vector2<T> pos, Vector2<T> size)
			: x(pos.x), y(pos.y), width(size.x), height(size.y) {}

		Vector2<T> position() const {
			return Vector2<T>(x, y);
		}

		Vector2<T> size() const {
			return Vector2<T>(width, height);
		}

		float area() const {
			return width * height;
		}

		void set_position(const Vector2<T>& position) {
			x = position.x;
			y = position.y;
		}

		void set_size(const Vector2<T>& size) {
			width = size.x;
			height = size.y;
		}

		T x, y, width, height;
	};
}
