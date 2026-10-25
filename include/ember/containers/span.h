#pragma once

// Borrowed contiguous storage, passed by value as a pointer and count. Keep the elements alive
// and their addresses valid for every use. A const Span still permits writes to mutable T;
// use Span<const T> for read-only elements. Access preconditions are checked by assertions.

#include <ember/core/macros.h>

#include <cstddef>
#include <initializer_list>
#include <type_traits>

namespace ember
{
	template <typename T>
	class Span;

	namespace Private
	{
		template <typename T>
		inline constexpr bool is_span_v = false;

		template <typename T>
		inline constexpr bool is_span_v<Span<T>> = true;

		// Array-pointer conversion permits added qualifiers but rejects Derived[] as Base[].
		template <typename From, typename To>
		concept SpanElementCompatible = std::is_convertible_v<From (*)[], To (*)[]>;

		// Member checks cannot prove that data() points to size() contiguous elements.
		template <typename C, typename T>
		concept SpanCompatibleContainer =
		    !is_span_v<std::remove_cvref_t<C>> && !std::is_array_v<std::remove_cvref_t<C>> &&
		    requires(C& c) {
			    requires std::is_pointer_v<decltype(c.data())>;
			    requires std::is_convertible_v<decltype(c.size()), size_t>;
			    requires SpanElementCompatible<std::remove_pointer_t<decltype(c.data())>, T>;
		    };
	}

	template <class T>
	class Span
	{
	public:
		using element_type = T;
		using value_type = std::remove_cv_t<T>;
		using size_type = size_t;
		using difference_type = ptrdiff_t;
		using pointer = T*;
		using reference = T&;
		using iterator = T*;

		constexpr Span() = default;

		// data may be null only when count is zero.
		EMBER_FINLINE constexpr Span(pointer data, size_type count) : m_data(data), m_size(count)
		{
			EMBER_ASSERT(data != nullptr || count == 0);
		}

		// first and last must form an ordered range in the same array, with distance fitting
		// ptrdiff_t. last may be one past the end.
		EMBER_FINLINE constexpr Span(pointer first, pointer last)
		    : m_data(first), m_size(static_cast<size_type>(last - first))
		{
			EMBER_ASSERT(first <= last);
		}

		// String literals include their terminating NUL in the count.
		template <size_t N>
		EMBER_FINLINE constexpr Span(T (&array)[N]) : m_data(array), m_size(N)
		{
		}

		// Rvalues are accepted for const elements, but the span must not outlive their storage.
		template <typename Container>
		    requires(Private::SpanCompatibleContainer<Container, T> &&
		                (std::is_lvalue_reference_v<Container> || std::is_const_v<T>))
		EMBER_FINLINE constexpr Span(Container&& container EMBER_LIFETIMEBOUND)
		    : m_data(container.data()), m_size(static_cast<size_type>(container.size()))
		{
		}

		// A directly supplied braced list expires at the end of the full expression.
		// Storing this span does not extend the backing array's lifetime.
		EMBER_FINLINE constexpr Span(std::initializer_list<value_type> list EMBER_LIFETIMEBOUND)
		    requires(std::is_const_v<T>)
		    : Span(list.begin(), list.size())
		{
		}

		template <typename U>
		    requires(Private::SpanElementCompatible<U, T>)
		EMBER_FINLINE constexpr Span(Span<U> other) : m_data(other.data()), m_size(other.size())
		{
		}

		constexpr Span(const Span&) = default;
		constexpr Span& operator=(const Span&) = default;

		constexpr pointer data() const
		{
			return m_data;
		}

		EMBER_FINLINE constexpr size_type size() const
		{
			return m_size;
		}

		EMBER_FINLINE constexpr size_type size_bytes() const
		{
			return m_size * sizeof(T);
		}

		EMBER_FINLINE constexpr bool empty() const
		{
			return m_size == 0;
		}

		// front() and back() require a nonempty span.
		EMBER_FINLINE constexpr reference operator[](size_type index) const
		{
			EMBER_ASSERT(index < m_size);
			return m_data[index];
		}

		EMBER_FINLINE constexpr reference front() const
		{
			EMBER_ASSERT(m_size != 0);
			return m_data[0];
		}

		EMBER_FINLINE constexpr reference back() const
		{
			EMBER_ASSERT(m_size != 0);
			return m_data[m_size - 1];
		}

		EMBER_FINLINE constexpr iterator begin() const
		{
			return m_data;
		}

		EMBER_FINLINE constexpr iterator end() const
		{
			return m_data + m_size;
		}

		EMBER_FINLINE constexpr Span first(size_type count) const
		{
			EMBER_ASSERT(count <= m_size);
			return Span(m_data, count);
		}

		EMBER_FINLINE constexpr Span last(size_type count) const
		{
			EMBER_ASSERT(count <= m_size);
			return Span(m_data + (m_size - count), count);
		}

		EMBER_FINLINE constexpr Span subspan(size_type offset, size_type count) const
		{
			EMBER_ASSERT(offset <= m_size);

			// Subtraction avoids overflow in offset + count.
			EMBER_ASSERT(count <= m_size - offset);

			return Span(m_data + offset, count);
		}

		EMBER_FINLINE constexpr Span subspan(size_type offset) const
		{
			EMBER_ASSERT(offset <= m_size);
			return Span(m_data + offset, m_size - offset);
		}

	private:
		pointer m_data = nullptr;
		size_type m_size = 0;
	};

	template <typename T, size_t N>
	Span(T (&)[N]) -> Span<T>;

	// Braced values deduce const elements; the array expires at full-expression end.
	template <typename T>
	Span(std::initializer_list<T>) -> Span<const T>;

	// Use Span(container) to deduce a view of the container's elements. Span{container} can select
	// the initializer-list guide instead, treating the container itself as one element.
	template <typename Container>
	    requires(requires(Container& c) {
		    c.data();
		    c.size();
	    })
	Span(Container&&) -> Span<std::remove_pointer_t<decltype(std::declval<Container&>().data())>>;

	// Borrows the same storage as an object-representation view.
	template <typename T>
	EMBER_FINLINE Span<const std::byte> as_bytes(Span<T> s)
	{
		return Span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size_bytes());
	}

	template <typename T>
	    requires(!std::is_const_v<T>)
	EMBER_FINLINE Span<std::byte> as_writable_bytes(Span<T> s)
	{
		return Span<std::byte>(reinterpret_cast<std::byte*>(s.data()), s.size_bytes());
	}
}
