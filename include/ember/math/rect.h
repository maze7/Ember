#pragma once

#include <type_traits>

#include "math.h"

namespace Ember
{
	template <class T> requires std::is_arithmetic_v<T>
	struct Rect
	{
		constexpr Rect() : x(0), y(0), w(0), h(0) {}

		constexpr Rect(T x, T y, T width, T height)
			: x(x), y(y), w(width), h(height) {}

		constexpr Rect(glm::vec<2, T> pos, glm::vec<2, T> size)
			: x(pos.x), y(pos.y), w(size.x), h(size.y) {}

		constexpr glm::vec<2, T> position() const {
			return glm::vec<2, T>(x, y);
		}

		constexpr glm::vec<2, T> size() const {
			return glm::vec<2, T>(w, h);
		}

		[[nodiscard]] constexpr float area() const {
			return w * h;
		}

		constexpr void set_position(const glm::vec<2, T>& position) {
			x = position.x;
			y = position.y;
		}

		constexpr void set_size(const glm::vec<2, T>& size) {
			w = size.x;
			h = size.y;
		}

		constexpr Rect<T> operator+(const glm::vec<2, T>& rhs) const {
			return Rect(x + rhs.x, y + rhs.y, w, h);
		}

		constexpr Rect<T> operator-(const glm::vec<2, T>& rhs) const {
			return Rect(x - rhs.x, y - rhs.y, w, h);
		}

		constexpr Rect<T>& operator+=(const glm::vec<2, T>& rhs) {
			x += rhs.x;
			y += rhs.y;
			return *this;
		}

		constexpr Rect<T>& operator-=(const glm::vec<2, T>& rhs) {
			x -= rhs.x;
			y -= rhs.y;
			return *this;
		}

		constexpr bool operator==(const Rect& rhs) const {
			return Math::abs(x - rhs.x) < Math::EPSILON && Math::abs(y - rhs.y) < Math::EPSILON && Math::abs(w - rhs.w) < Math::EPSILON && Math::abs(h - rhs.h) < Math::EPSILON;
		}

		constexpr bool operator!=(const Rect& rhs) const {
			return !(*this == rhs);
		};

		constexpr T left() const { return x; }
		constexpr T right() const { return x + w; }
		constexpr T top() const { return y; }
		constexpr T bottom() const { return y + h; }

		constexpr glm::vec<2, T> center() const { return glm::vec<2, T>(x + w / 2, y + h / 2); }
		constexpr T center_x() const { return x + w / 2; }
		constexpr T center_y() const { return y + h / 2; }

		constexpr glm::vec<2, T> top_left() const { return glm::vec<2, T>(x, y); }
		constexpr glm::vec<2, T> top_right() const { return glm::vec<2, T>(x + w, y); }
		constexpr glm::vec<2, T> bottom_right() const { return glm::vec<2, T>(x + w, y + h); }
		constexpr glm::vec<2, T> bottom_left() const { return glm::vec<2, T>(x, y + h); }

		constexpr glm::vec<2, T> center_left() const { return glm::vec<2, T>(x, y + h / 2); }
		constexpr glm::vec<2, T> center_right() const { return glm::vec<2, T>(x + w, y + h / 2); }
		constexpr glm::vec<2, T> middle_top() const { return glm::vec<2, T>(x + w / 2, y); }
		constexpr glm::vec<2, T> middle_bottom() const { return glm::vec<2, T>(x + w / 2, y + h); }

		constexpr bool contains(const glm::vec<2, T>& pt) const {
			return pt.x >= x && pt.x < x + w && pt.y >= y && pt.y < y + h;
		}

		constexpr bool contains(const Rect& rect) const {
			return rect.x >= x && rect.x + rect.w < x + w && rect.y >= y && rect.y + rect.h < y + h;
		}

		constexpr bool overlaps(const Rect& rect) const {
			return x + w > rect.x && y + h > rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
		}

		constexpr Rect<T> overlap_rect(const Rect& against) const {
			Rect result;

			if (x + w >= against.x && x < against.x + against.w)
			{
				result.x = Math::max(x, against.x);
				result.w = Math::min(x + w, against.x + against.w) - result.x;
			}

			if (y + h >= against.y && y < against.y + against.h)
			{
				result.y = Math::max(y, against.y);
				result.h = Math::min(y + h, against.y + against.h) - result.y;
			}

			return result;
		}

		constexpr Rect<T> scale(T s) const {
			return Rect<T>(x * s, y * s, w * s, h * s);
		}

		constexpr Rect<T> scale(T sx, T sy) const {
			return Rect<T>(x * sx, y * sy, w * sx, h * sy);
		}

		constexpr Rect<T> inflate(T amount) const {
			return Rect(x - amount, y - amount, w + amount * 2, h + amount * 2);
		}

		constexpr Rect<T> inflate(T amount_x, T amount_y) const {
			return Rect(x - amount_x, y - amount_y, w + amount_x * 2, h + amount_y * 2);
		}

		T x, y, w, h;
	};

	using Rectf = Rect<float>;
	using Recti = Rect<int>;
}
