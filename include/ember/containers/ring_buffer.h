#pragma once

// The last N values of a numbered series, in storage allocated once. Values are numbered in the
// order they are pushed, from zero, and the buffer holds the window [first, next): a push claims
// the next number and evicts the value capacity behind it, a pop drops the oldest. A value is found
// by its number while it is inside the window, and iteration walks the window oldest to newest.
//
// Slots are constructed at initialize() and destroyed at shutdown(), and never move. A push hands
// back the slot for the caller to fill and constructs nothing, so a value that owns memory keeps
// it across reuse and a value that holds an atomic is never copied. The frame history is one of
// these, a netcode's tick history is another, and a frame-time graph reads one through data()
// and offset(). Not thread safe: whoever pushes owns the window.
//
// Costs. A push or a find is a modulo by the capacity, which need not be a power of two.

#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <new>
#include <source_location>
#include <type_traits>

namespace ember
{
	template <typename T>
	class RingBuffer final
	{
		static_assert(std::is_default_constructible_v<T>, "ring slots are built at initialize");
		static_assert(std::is_nothrow_destructible_v<T>, "ring values are destroyed at shutdown");

	public:
		// Walks the window oldest to newest. sequence() is the number of the value under it.
		template <typename Ref>
		class Iter
		{
		public:
			Ref operator*() const
			{
				return m_ring->m_slots[m_ring->slot_of(m_sequence)];
			}

			u64 sequence() const
			{
				return m_sequence;
			}

			Iter& operator++()
			{
				++m_sequence;
				return *this;
			}

			bool operator==(const Iter&) const = default;

		private:
			friend RingBuffer;

			Iter(const RingBuffer* ring, u64 sequence) : m_ring(ring), m_sequence(sequence)
			{
			}

			const RingBuffer* m_ring = nullptr;
			u64 m_sequence = 0;
		};

		using Iterator = Iter<T&>;
		using ConstIterator = Iter<const T&>;

		RingBuffer() = default;

		~RingBuffer()
		{
			shutdown();
		}

		RingBuffer(const RingBuffer&) = delete;
		RingBuffer& operator=(const RingBuffer&) = delete;

		// Takes [capacity] slots, at least one, through [allocator] under [tag], and builds a
		// value in each. Runs once. The allocator must outlive the buffer.
		void initialize(memory::Allocator* allocator, memory::Tag tag, u32 capacity)
		{
			EMBER_ASSERT_MSG(m_slots == nullptr, "initialize runs once");
			EMBER_ASSERT(allocator != nullptr);
			EMBER_ASSERT(capacity >= 1);

			m_allocator = allocator;
			m_slots = (T*)allocator->allocate(tag, capacity * sizeof(T), alignof(T),
			    std::source_location{});
			for (u32 i = 0; i < capacity; ++i)
			{
				::new ((void*)(m_slots + i)) T;
			}

			m_capacity = capacity;
			m_first = 0;
			m_next = 0;
		}

		// Destroys every slot and returns the storage.
		void shutdown()
		{
			if (m_slots == nullptr)
			{
				return;
			}

			for (u32 i = 0; i < m_capacity; ++i)
			{
				m_slots[i].~T();
			}

			m_allocator->deallocate(m_slots, m_capacity * sizeof(T), alignof(T));
			m_allocator = nullptr;
			m_slots = nullptr;
			m_capacity = 0;
			m_first = 0;
			m_next = 0;
		}

		// Claims the slot for the next number, evicting the value capacity behind it, and hands
		// it back holding whatever it held before, for the caller to fill.
		T& push()
		{
			EMBER_ASSERT_MSG(m_slots != nullptr, "push before initialize");

			if (size() == m_capacity)
			{
				++m_first;
			}

			return m_slots[slot_of(m_next++)];
		}

		// Drops the oldest value. Its slot keeps what it held until a push reuses it.
		void pop()
		{
			EMBER_ASSERT_MSG(!empty(), "pop on an empty ring");
			++m_first;
		}

		// Forgets every value and restarts the numbering at zero. Slots keep what they held.
		void clear()
		{
			m_first = 0;
			m_next = 0;
		}

		u32 size() const
		{
			return (u32)(m_next - m_first);
		}

		bool empty() const
		{
			return m_next == m_first;
		}

		bool full() const
		{
			return size() == m_capacity;
		}

		u32 capacity() const
		{
			return m_capacity;
		}

		// The window: the oldest value's number and the number the next push takes.
		u64 first_sequence() const
		{
			return m_first;
		}

		u64 next_sequence() const
		{
			return m_next;
		}

		// The value with this number, or null when it is outside the window.
		T* find(u64 sequence)
		{
			return holds(sequence) ? m_slots + slot_of(sequence) : nullptr;
		}

		const T* find(u64 sequence) const
		{
			return holds(sequence) ? m_slots + slot_of(sequence) : nullptr;
		}

		T& front()
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_first)];
		}

		const T& front() const
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_first)];
		}

		T& back()
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_next - 1)];
		}

		const T& back() const
		{
			EMBER_ASSERT(!empty());
			return m_slots[slot_of(m_next - 1)];
		}

		// The slots as laid out and where the oldest value sits in them, for a plot that reads
		// the raw array: size() values from offset() on, wrapping at capacity().
		const T* data() const
		{
			return m_slots;
		}

		u32 offset() const
		{
			return slot_of(m_first);
		}

		Iterator begin()
		{
			return Iterator(this, m_first);
		}

		Iterator end()
		{
			return Iterator(this, m_next);
		}

		ConstIterator begin() const
		{
			return ConstIterator(this, m_first);
		}

		ConstIterator end() const
		{
			return ConstIterator(this, m_next);
		}

	private:
		u32 slot_of(u64 sequence) const
		{
			return (u32)(sequence % m_capacity);
		}

		bool holds(u64 sequence) const
		{
			return sequence >= m_first && sequence < m_next;
		}

		memory::Allocator* m_allocator = nullptr;
		T* m_slots = nullptr;
		u32 m_capacity = 0;

		// The oldest value held, and the number the next push takes.
		u64 m_first = 0;
		u64 m_next = 0;
	};
}
