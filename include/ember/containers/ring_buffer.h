#pragma once

// Fixed storage for the last N values of a numbered series. The retained range is [first, next),
// iterated oldest to newest. Slots are built at initialize() and destroyed at shutdown(); push,
// pop, and clear change the window without reconstructing values, preserving nested storage
// and allowing non-copyable values such as frame records with atomics. Not thread safe.

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

		// Builds capacity slots, at least one. Keep the allocator alive through shutdown.
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

		// Evicts the oldest entry when full. The returned slot retains its previous contents;
		// the caller must prepare it for the new value.
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

		// Physical storage order. Read size() entries from offset(), wrapping at capacity().
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

		u64 m_first = 0;
		u64 m_next = 0;
	};
}
