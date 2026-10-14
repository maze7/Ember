#pragma once

// Single ownership of an object made by new_object. A Unique remembers the allocator, so the
// object goes back exactly where it came from with no category to repeat at the release site,
// and a Unique made in an arena costs its destructor and nothing else.
//
// It is Ember's own rather than std::unique_ptr with a deleter because the deleter form drags
// <memory> into every header that holds one, and because the whole type is forty lines whose
// behaviour the engine wants to own: no custom deleters, no arrays, no conversion between
// element types, which would hand delete_object a size that is not the object's.

#include <ember/memory/allocator.h>
#include <ember/memory/memory.h>

#include <type_traits>
#include <utility>

namespace ember
{
	template <typename T>
	class Unique
	{
	public:
		Unique() = default;

		// Takes ownership of [object], which must have come from new_object through
		// [allocator]. make_unique is the usual way to get here.
		Unique(T* object, Allocator* allocator) : m_object(object), m_allocator(allocator)
		{
		}

		~Unique()
		{
			reset();
		}

		Unique(const Unique&) = delete;
		Unique& operator=(const Unique&) = delete;

		// Moves are noexcept so that a container of Uniques relocates by moving rather than by
		// copying, which it could not do anyway.
		Unique(Unique&& other) noexcept : m_object(other.m_object), m_allocator(other.m_allocator)
		{
			other.m_object = nullptr;
		}

		Unique& operator=(Unique&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				m_object = other.m_object;
				m_allocator = other.m_allocator;
				other.m_object = nullptr;
			}

			return *this;
		}

		T* get() const
		{
			return m_object;
		}

		T* operator->() const
		{
			EMBER_ASSERT(m_object != nullptr);
			return m_object;
		}

		T& operator*() const
		{
			EMBER_ASSERT(m_object != nullptr);
			return *m_object;
		}

		explicit operator bool() const
		{
			return m_object != nullptr;
		}

		// Gives the object up without destroying it. The caller now owns it and must
		// delete_object it through the same allocator.
		T* release()
		{
			T* object = m_object;
			m_object = nullptr;
			return object;
		}

		// Destroys the object, if any, and becomes empty.
		void reset()
		{
			if (m_object != nullptr)
			{
				memory::delete_object(m_allocator, m_object);
				m_object = nullptr;
			}
		}

	private:
		T* m_object = nullptr;
		Allocator* m_allocator = nullptr;
	};

	namespace memory
	{
		template <typename T, typename... Args>
		[[nodiscard]] Unique<T> make_unique(Allocator* allocator, Args&&... args)
		{
			static_assert(!std::is_array_v<T>, "a dynamic array is a Vector, not a Unique");

			return Unique<T>(new_object<T>(allocator, std::forward<Args>(args)...), allocator);
		}
	}
}
