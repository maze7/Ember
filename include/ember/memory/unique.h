#pragma once

// Owns one object and remembers which allocator must receive its storage.
// The allocator must outlive the object.
//
// This small wrapper keeps <memory> out of headers that own engine objects.
// Arrays, custom deleters, and conversions between element types are left out.
// Converting to a base type could give delete_object the wrong size or alignment.

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

		// Moving only transfers pointers, so container relocation cannot throw here.
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

		// Destroys the object, if any, and leaves this Unique empty.
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
