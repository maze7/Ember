#pragma once

// A bounded queue that any number of threads may push to and pop from at once, following Dmitry
// Vyukov's design. The cells are allocated once at initialize(), through an Allocator under a tag,
// and nothing is allocated after.
//
// Every cell carries a sequence number that says whose turn it is. A producer holding a ticket t
// may fill cell t & mask once the cell's sequence equals t; filling it publishes sequence t + 1,
// which is what the consumer holding ticket t waits for, and consuming publishes t + capacity for
// the producer of the next lap. Tickets are claimed with a compare-exchange on the push or pop
// position, so no two threads ever touch one cell in the same role, and values are published
// through the sequence alone. A release store on one side, an acquire load on the other. Lock free,
// no allocation, and no ABA problem because a ticket is never reused within a lap.

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

		// Takes [capacity] cells, a power of two of at least two, through [allocator] under [tag].
		// Runs once, before any thread pushes or pops. The allocator must outlive the queue.
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

			// Cell i starts on ticket i, so the first lap's producers find their turn waiting.
			for (size_t index = 0; index < capacity; ++index)
			{
				Cell* cell = ::new ((void*)(m_cells + index)) Cell;
				cell->sequence.store(index, std::memory_order_relaxed);
			}

			m_push.store(0, std::memory_order_relaxed);
			m_pop.store(0, std::memory_order_relaxed);
		}

		// Returns the cells. Whatever was still queued is dropped, and the cells hold nothing
		// that needs destroying. For teardown, once every producer and consumer has stopped.
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

		// Copies [value] in. False when the queue is full at this instant, or looks it, see
		// above; the value is then still the caller's.
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
					// Our turn at this cell, if no other producer takes the ticket first.
					if (m_push.compare_exchange_weak(ticket, ticket + 1, std::memory_order_relaxed))
					{
						break;
					}
				}
				else if (turn < 0)
				{
					// The consumer of the previous lap has not emptied this cell: full.
					return false;
				}
				else
				{
					// Another producer took the ticket while we looked; start again from the
					// position it left.
					ticket = m_push.load(std::memory_order_relaxed);
				}
			}

			cell->value = value;
			cell->sequence.store(ticket + 1, std::memory_order_release);
			return true;
		}

		// Copies the oldest value out. False when the queue is empty at this instant, or looks
		// it, see above.
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
					// The producer of this lap has not filled this cell: empty.
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

		// Distance between the two positions at the moment of the call. Claimed but unpublished
		// cells count, so this is a hint for stats and idle decisions, not a promise that the
		// next push or pop succeeds.
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

		// A cache line each: producers hammer one position and consumers the other, and neither
		// should drag the other's line back and forth.
		alignas(EMBER_CACHE_LINE) std::atomic<size_t> m_push{0};
		alignas(EMBER_CACHE_LINE) std::atomic<size_t> m_pop{0};
	};
}
