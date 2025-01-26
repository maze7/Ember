#pragma once

#include <type_traits>

namespace Ember
{
	template <class T> requires std::is_arithmetic_v<T>
	struct Quad
	{
		Quad() : x(0), y(0), width(0), height(0) {}

		Quad(T x, T y, T width, T height)
			: x(x), y(y), width(width), height(height) {}

		Quad(glm::vec<2, T> pos, glm::vec<2, T> size)
			: x(pos.x), y(pos.y), width(size.x), height(size.y) {}

		glm::vec<2, T> position() const {
			return glm::vec<2, T>(x, y);
		}

		glm::vec<2, T> size() const {
			return glm::vec<2, T>(width, height);
		}

		[[nodiscard]] float area() const {
			return width * height;
		}

		void set_position(const glm::vec<2, T>& position) {
			x = position.x;
			y = position.y;
		}

		void set_size(const glm::vec<2, T>& size) {
			width = size.x;
			height = size.y;
		}

		T x, y, width, height;
	};

	template <class T>
	using Rect = Quad<T>;
}
