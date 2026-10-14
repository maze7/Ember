#pragma once

// A non-owning view of contiguous elements. Copying a Span copies a pointer and an element count,
// leaving the elements in their original storage.
//
// The caller must keep that storage alive and its addresses valid for every use of the span. A
// stored span needs the same guarantee.
//
// Making the span const does not make its elements const. Use Span<const T> when the elements must
// be read-only.
//
// A function taking Span<const u32> can accept a braced list:
//
//     consume({1u, 2u, 3u});
//
// Consume the list within the same full expression. Its backing array is temporary, and copying the
// span does not keep that array alive.
//
// Extent is always dynamic. Iterators are raw pointers. Access preconditions are checked when
// EMBER_ASSERTS_ENABLED is true.

#include <ember/core/macros.h>

#include <cstddef>
#include <initializer_list>
#include <iterator>
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

		// Check compatibility as pointers to arrays. Derived* can convert to Base*, but an array of
		// Derived is not an array of Base. The array conversion preserves element identity while
		// allowing added qualifiers.
		template <typename From, typename To>
		concept SpanElementCompatible = std::is_convertible_v<From (*)[], To (*)[]>;

		// Recognize containers through data() and size() without depending on <ranges>. These
		// checks establish the member types; the container must still provide size() contiguous
		// elements starting at data().
		//
		// Spans and raw arrays have dedicated constructors.
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
		// Use standard container names so generic code can inspect the element, iterator and size
		// types.
		using element_type = T;
		using value_type = std::remove_cv_t<T>;
		using size_type = size_t;
		using difference_type = ptrdiff_t;
		using pointer = T*;
		using reference = T&;
		using iterator = T*;
		using reverse_iterator = std::reverse_iterator<iterator>;

		// Constructs an empty view with a null data pointer.
		constexpr Span() = default;

		// Borrows [count] contiguous elements starting at [data]. A null pointer is valid only when
		// [count] is zero.
		EMBER_FINLINE constexpr Span(pointer data, size_type count) : m_data(data), m_size(count)
		{
			EMBER_ASSERT(data != nullptr || count == 0);
		}

		// Borrows [first, last). The pointers must describe an ordered range in the same array, or
		// both be null. A one-past-the-end position is allowed. The distance must fit in ptrdiff_t.
		EMBER_FINLINE constexpr Span(pointer first, pointer last)
		    : m_data(first), m_size(static_cast<size_type>(last - first))
		{
			EMBER_ASSERT(first <= last);
		}

		// Borrows an array and deduces its element count. String literals include the terminating
		// null character.
		template <size_t N>
		EMBER_FINLINE constexpr Span(T (&array)[N]) : m_data(array), m_size(N)
		{
		}

		// Borrows a container's contiguous storage. Rvalues are accepted only for const elements,
		// but that does not extend the container's lifetime. If the container owns its storage,
		// consume the span before it is destroyed.
		template <typename Container>
		    requires(Private::SpanCompatibleContainer<Container, T> &&
		                (std::is_lvalue_reference_v<Container> || std::is_const_v<T>))
		EMBER_FINLINE constexpr Span(Container&& container EMBER_LIFETIMEBOUND)
		    : m_data(container.data()), m_size(static_cast<size_type>(container.size()))
		{
		}

		// Borrows the initializer list's backing array. Its elements are const, so this constructor
		// is available only for spans of const elements.
		//
		// A braced list passed directly here lasts until the end of the full expression. Storing
		// the span does not extend that lifetime.
		EMBER_FINLINE constexpr Span(std::initializer_list<value_type> list EMBER_LIFETIMEBOUND)
		    requires(std::is_const_v<T>)
		    : Span(list.begin(), list.size())
		{
		}

		// Allows qualification conversions, such as Span<T> to Span<const T>. Element compatibility
		// prevents changing the element type or removing qualifiers.
		template <typename U>
		    requires(Private::SpanElementCompatible<U, T>)
		EMBER_FINLINE constexpr Span(Span<U> other) : m_data(other.data()), m_size(other.size())
		{
		}

		// Pass spans by value. Copying copies only the pointer and count; both views continue to
		// refer to the same elements.
		constexpr Span(const Span&) = default;
		constexpr Span& operator=(const Span&) = default;

		EMBER_FINLINE constexpr pointer data() const
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

		// Element access requires a valid index. front() and back() require a nonempty span. These
		// preconditions are checked when assertions are enabled.
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

		EMBER_FINLINE constexpr reverse_iterator rbegin() const
		{
			return reverse_iterator(end());
		}

		EMBER_FINLINE constexpr reverse_iterator rend() const
		{
			return reverse_iterator(begin());
		}

		// Returns a view of the first [count] elements. [count] must not exceed size().
		EMBER_FINLINE constexpr Span first(size_type count) const
		{
			EMBER_ASSERT(count <= m_size);
			return Span(m_data, count);
		}

		// Returns a view of the last [count] elements. [count] must not exceed size().
		EMBER_FINLINE constexpr Span last(size_type count) const
		{
			EMBER_ASSERT(count <= m_size);
			return Span(m_data + (m_size - count), count);
		}

		// Returns [count] elements starting at [offset]. [offset] may equal size(), but [count]
		// must fit within the remaining elements.
		EMBER_FINLINE constexpr Span subspan(size_type offset, size_type count) const
		{
			EMBER_ASSERT(offset <= m_size);

			// Compare against the remaining size so a large [count] cannot make offset + count wrap
			// and pass the check.
			EMBER_ASSERT(count <= m_size - offset);

			return Span(m_data + offset, count);
		}

		// Returns the elements from [offset] to the end. [offset] must not exceed size().
		EMBER_FINLINE constexpr Span subspan(size_type offset) const
		{
			EMBER_ASSERT(offset <= m_size);
			return Span(m_data + offset, m_size - offset);
		}

		// View identity and element equality are different operations, so neither is chosen for
		// operator==. Compare data() and size() for identity, or use std::ranges::equal for
		// contents.

	private:
		pointer m_data = nullptr;
		size_type m_size = 0;
	};

	template <typename T, size_t N>
	Span(T (&)[N]) -> Span<T>;

	// Braced values deduce a view of const elements: Span{1, 2, 3} deduces Span<const int>. The
	// temporary array still has full-expression lifetime.
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

	// Views the elements' object representation without copying. The returned span borrows the same
	// storage.
	template <typename T>
	EMBER_FINLINE Span<const std::byte> as_bytes(Span<T> s)
	{
		return Span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size_bytes());
	}

	// Provides writable access to the same bytes. Const elements are excluded so a byte view cannot
	// bypass their constness.
	template <typename T>
	    requires(!std::is_const_v<T>)
	EMBER_FINLINE Span<std::byte> as_writable_bytes(Span<T> s)
	{
		return Span<std::byte>(reinterpret_cast<std::byte*>(s.data()), s.size_bytes());
	}
}
