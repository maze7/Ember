#pragma once

#include <type_traits>

namespace Ember
{
	template <class T> requires std::is_arithmetic_v<T>
	struct Rect
	{
		Rect() : x(0), y(0), w(0), h(0) {}

		Rect(T x, T y, T width, T height)
			: x(x), y(y), w(width), h(height) {}

		Rect(glm::vec<2, T> pos, glm::vec<2, T> size)
			: x(pos.x), y(pos.y), w(size.x), h(size.y) {}

		glm::vec<2, T> position() const {
			return glm::vec<2, T>(x, y);
		}

		glm::vec<2, T> size() const {
			return glm::vec<2, T>(w, h);
		}

		[[nodiscard]] float area() const {
			return w * h;
		}

		void set_position(const glm::vec<2, T>& position) {
			x = position.x;
			y = position.y;
		}

		void set_size(const glm::vec<2, T>& size) {
			w = size.x;
			h = size.y;
		}

		T x, y, w, h;
	};

	using Rectf = Rect<float>;
	using Recti = Rect<int>;
}
