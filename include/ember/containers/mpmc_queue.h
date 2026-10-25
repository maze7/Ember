#pragma once

// Bounded multi-producer, multi-consumer queue based on Dmitry Vyukov's sequence-number design.
// Storage is fixed at initialize(). Tickets assign cells; release/acquire sequence updates
// publish values and permit reuse. A paused ticket owner can hold up the opposite side, so
// a failed operation does not prove that the whole queue is full or empty.

#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <source_location>
#include <type_traits>

namespace ember
{
	template <typename T>
	class MpmcQueue final
	{
		static_assert(std::is_trivially_copyable_v<T>, "queue values are copied as bytes");
		static_assert(std::is_default_constructible_v<T>, "queue cells are built empty");

	public:
		MpmcQueue() = default;

		~MpmcQueue()
		{
			shutdown();
		}

		MpmcQueue(const MpmcQueue&) = delete;
		MpmcQueue& operator=(const MpmcQueue&) = delete;

		// capacity must be a power of two, above zero. Initialize before starting producers
		// or consumers, and keep the allocator alive through shutdown.
		void initialize(memory::Allocator* allocator, memory::Tag tag, size_t capacity)
		{
			EMBER_ASSERT_MSG(m_cells == nullptr, "initialize runs once");
			EMBER_ASSERT(allocator != nullptr);
			EMBER_ASSERT(capacity >= 2 && std::has_single_bit(capacity));

			m_allocator = allocator;
			m_cells = (Cell*)allocator->allocate(tag, capacity * sizeof(Cell), alignof(Cell),
			    std::source_location{});
			m_mask = capacity - 1;
			m_capacity = capacity;

			for (size_t index = 0; index < capacity; ++index)
			{
				Cell* cell = ::new ((void*)(m_cells + index)) Cell;
				cell->sequence.store(index, std::memory_order_relaxed);
			}

			m_push.store(0, std::memory_order_relaxed);
			m_pop.store(0, std::memory_order_relaxed);
		}

		// Stop every producer and consumer first. Remaining queued values are discarded.
		void shutdown()
		{
			if (m_cells == nullptr)
			{
				return;
			}

			m_allocator->deallocate(m_cells, m_capacity * sizeof(Cell), alignof(Cell));
			m_cells = nullptr;
			m_allocator = nullptr;
			m_mask = 0;
			m_capacity = 0;
		}

		// Returns false if the next cell is unavailable; value is never modified.
		[[nodiscard]] bool try_push(const T& value)
		{
			EMBER_ASSERT_MSG(m_cells != nullptr, "push before initialize");

			Cell* cell = nullptr;
			size_t ticket = m_push.load(std::memory_order_relaxed);

			for (;;)
			{
				cell = m_cells + (ticket & m_mask);

				const size_t sequence = cell->sequence.load(std::memory_order_acquire);
				const ptrdiff_t turn = (ptrdiff_t)sequence - (ptrdiff_t)ticket;

				if (turn == 0)
				{
					if (m_push.compare_exchange_weak(ticket, ticket + 1, std::memory_order_relaxed))
					{
						break;
					}
				}
				else if (turn < 0)
				{
					// This cell has not yet been released by its previous consumer.
					return false;
				}
				else
				{
					ticket = m_push.load(std::memory_order_relaxed);
				}
			}

			cell->value = value;
			cell->sequence.store(ticket + 1, std::memory_order_release);
			return true;
		}

		// Returns false if the next cell is unpublished, leaving out unchanged.
		[[nodiscard]] bool try_pop(T& out)
		{
			EMBER_ASSERT_MSG(m_cells != nullptr, "pop before initialize");

			Cell* cell = nullptr;
			size_t ticket = m_pop.load(std::memory_order_relaxed);

			for (;;)
			{
				cell = m_cells + (ticket & m_mask);

				const size_t sequence = cell->sequence.load(std::memory_order_acquire);
				const ptrdiff_t turn = (ptrdiff_t)sequence - (ptrdiff_t)(ticket + 1);

				if (turn == 0)
				{
					if (m_pop.compare_exchange_weak(ticket, ticket + 1, std::memory_order_relaxed))
					{
						break;
					}
				}
				else if (turn < 0)
				{
					// The owner of this ticket has not published a value.
					return false;
				}
				else
				{
					ticket = m_pop.load(std::memory_order_relaxed);
				}
			}

			out = cell->value;
			cell->sequence.store(ticket + m_capacity, std::memory_order_release);
			return true;
		}

		size_t capacity() const
		{
			return m_capacity;
		}

		// Approximate ticket distance, including unpublished cells. Counts are sampled separately;
		// use for telemetry, not to decide whether a push or pop will succeed.
		size_t size_hint() const
		{
			const size_t push = m_push.load(std::memory_order_relaxed);
			const size_t pop = m_pop.load(std::memory_order_relaxed);
			return push >= pop ? push - pop : 0;
		}

	private:
		struct Cell
		{
			std::atomic<size_t> sequence;
			T value;
		};

		memory::Allocator* m_allocator = nullptr;
		Cell* m_cells = nullptr;
		size_t m_mask = 0;
		size_t m_capacity = 0;

		// Separate producer and consumer counters to reduce false sharing.
		alignas(EMBER_CACHE_LINE) std::atomic<size_t> m_push{0};
		alignas(EMBER_CACHE_LINE) std::atomic<size_t> m_pop{0};
	};
}
