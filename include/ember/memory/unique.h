#pragma once

// Owns one object and returns its storage to the original allocator, which must
// outlive it. Conversions to base types are unsupported because delete_object
// needs the original size and alignment.
//
// Arrays and custom deleters are unsupported.

#include <ember/core/macros.h>
#include <ember/memory/allocator.h>
#include <ember/memory/new.h>

#include <type_traits>
#include <utility>

namespace ember
{
	template <typename T>
	class Unique
	{
	public:
		Unique() = default;

		// Takes ownership of an object created by new_object through this allocator.
		// Use make_unique to allocate the object and establish ownership together.
		Unique(T* object, memory::Allocator* allocator) : m_object(object), m_allocator(allocator)
		{
		}

		~Unique()
		{
			reset();
		}

		Unique(const Unique&) = delete;
		Unique& operator=(const Unique&) = delete;

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

		// Gives up ownership without destroying the object. The caller must eventually
		// pass it to delete_object through the original allocator.
		T* release()
		{
			T* object = m_object;
			m_object = nullptr;
			return object;
		}

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
		memory::Allocator* m_allocator = nullptr;
	};

	namespace memory
	{
		template <typename T, typename... Args>
		[[nodiscard]] Unique<T> make_unique(Allocator* allocator, Tag tag, Args&&... args)
		{
			static_assert(!std::is_array_v<T>, "a dynamic array is a Vector, not a Unique");

			return Unique<T>(new_object<T>(allocator, tag, std::forward<Args>(args)...), allocator);
		}
	}
}
