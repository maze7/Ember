#pragma once

// Fixed-capacity object storage with generational handles. Slots never move; returned pointers
// remain usable only while their objects live. FIFO reuse delays generation recycling, but
// generations can wrap and let a stale handle match again. Exhaustion returns a null handle.
//
// Mutations and iteration require external synchronization. get() uses ordinary reads; exclude
// retirement and reuse of its target slot for the entire access. Iteration scans every slot.
// retire() destroys an object immediately; release() later makes its index available for reuse.

#include <ember/core/handle.h>
#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/memory/alignment.h>
#include <ember/memory/allocator.h>
#include <ember/memory/tag.h>

#include <cstddef>
#include <new>
#include <source_location>
#include <type_traits>
#include <utility>

namespace ember
{
	// Tag separates the public handle type from the stored implementation type.
	template <typename T, typename Tag = T>
	class Pool final
	{
		static_assert(std::is_nothrow_destructible_v<T>, "pool values are destroyed by erase");

	public:
		using Handle = ember::Handle<Tag>;

		static constexpr u32 MAX_CAPACITY = Handle::MAX_INDEX + 1;

		// Visits live slots in index order. Hold the pool's mutation lock while iterating.
		template <typename Ref>
		class Iter
		{
		public:
			Ref operator*() const
			{
				return m_pool->m_values[m_index];
			}

			Handle handle() const
			{
				return Handle::make(m_index, m_pool->m_generations[m_index]);
			}

			Iter& operator++()
			{
				++m_index;
				skip_dead();
				return *this;
			}

			bool operator==(const Iter&) const = default;

		private:
			friend Pool;

			Iter(const Pool* pool, u32 index) : m_pool(pool), m_index(index)
			{
				skip_dead();
			}

			void skip_dead()
			{
				while (m_index < m_pool->m_capacity && !m_pool->is_live(m_index))
				{
					++m_index;
				}
			}

			const Pool* m_pool = nullptr;
			u32 m_index = 0;
		};

		using Iterator = Iter<T&>;
		using ConstIterator = Iter<const T&>;

		Pool() = default;

		~Pool()
		{
			shutdown();
		}

		Pool(const Pool&) = delete;
		Pool& operator=(const Pool&) = delete;

		// Allocates fixed storage without constructing values. capacity is in [1, MAX_CAPACITY];
		// keep allocator alive through shutdown.
		void initialize(memory::Allocator* allocator, memory::Tag tag, u32 capacity)
		{
			EMBER_ASSERT_MSG(m_values == nullptr, "initialize runs once");
			EMBER_ASSERT(allocator != nullptr);
			EMBER_ASSERT(capacity >= 1 && capacity <= MAX_CAPACITY);

			const Layout layout = make_layout(capacity);
			u8* block =
			    (u8*)allocator->allocate(tag, layout.bytes, ALIGNMENT, std::source_location{});

			m_allocator = allocator;
			m_values = (T*)block;
			m_generations = (u16*)(block + layout.generations);
			m_ring = (u32*)(block + layout.ring);
			m_live = (u64*)(block + layout.live);
			m_bytes = layout.bytes;
			m_capacity = capacity;

			// Reserve generation zero so issued handles cannot equal the null sentinel.
			for (u32 index = 0; index < capacity; ++index)
			{
				m_generations[index] = 1;
			}

			for (u32 word = 0; word < live_words(capacity); ++word)
			{
				m_live[word] = 0;
			}

			reset_ring();
		}

		// Stop all users before destroying values and storage. Old handles must not survive
		// shutdown, even if the pool is later initialized again.
		void shutdown()
		{
			if (m_values == nullptr)
			{
				return;
			}

			clear();
			m_allocator->deallocate(m_values, m_bytes, ALIGNMENT);

			m_allocator = nullptr;
			m_values = nullptr;
			m_generations = nullptr;
			m_ring = nullptr;
			m_live = nullptr;
			m_bytes = 0;
			m_capacity = 0;
			m_ring_head = 0;
			m_free = 0;
			m_retired = 0;
		}

		// Returns a null handle when all slots are live or awaiting release.
		template <typename... Args>
		[[nodiscard]] Handle emplace(Args&&... args)
		{
			EMBER_ASSERT_MSG(m_values != nullptr, "emplace before initialize");

			if (m_free == 0)
			{
				return Handle{};
			}

			const u32 index = take_slot();
			::new ((void*)(m_values + index)) T(std::forward<Args>(args)...);
			m_live[index / 64] |= u64(1) << (index % 64);

			return Handle::make(index, m_generations[index]);
		}

		// Destroys the value and immediately releases its slot. Null or stale handles return false.
		bool erase(Handle handle)
		{
			if (!retire(handle))
			{
				return false;
			}

			release(handle.index());
			return true;
		}

		// Destroys the value and invalidates its handle, withholding the index until release().
		// Use when in-flight work still names an external table entry at that index. External
		// resources need separate retirement. Null or stale handles return false.
		bool retire(Handle handle)
		{
			const u32 index = find(handle);

			// A handle can match a retired slot's generation without ever having been issued. The
			// live bit keeps it from retiring the slot twice.
			if (index == NO_SLOT || !is_live(index))
			{
				return false;
			}

			kill(index);
			++m_retired;
			return true;
		}

		// Release a retired index exactly once, after all in-flight uses have finished.
		// The pool cannot verify that lifetime boundary.
		void release(u32 index)
		{
			EMBER_ASSERT(index < m_capacity);
			EMBER_ASSERT_MSG(!is_live(index), "release of a live slot");
			EMBER_ASSERT_MSG(m_retired != 0, "release without a retire");

			--m_retired;
			give_slot(index);
		}

		// Resolves an issued handle by bounds and generation. A returned pointer does not
		// prevent retirement; keep the object alive for the whole access.
		T* get(Handle handle)
		{
			const u32 index = find(handle);
			return index == NO_SLOT ? nullptr : m_values + index;
		}

		const T* get(Handle handle) const
		{
			const u32 index = find(handle);
			return index == NO_SLOT ? nullptr : m_values + index;
		}

		// Destroys live values and makes all slots available, including retired ones.
		// Finish every in-flight use first. Storage and advanced generations are retained.
		void clear()
		{
			for (u32 index = 0; index < m_capacity; ++index)
			{
				if (is_live(index))
				{
					kill(index);
				}
			}

			reset_ring();
			m_retired = 0;
		}

		// Slots in use: live plus retired.
		u32 size() const
		{
			return m_capacity - m_free;
		}

		// Slots retired and not yet released.
		u32 retired() const
		{
			return m_retired;
		}

		u32 capacity() const
		{
			return m_capacity;
		}

		bool empty() const
		{
			return m_free == m_capacity;
		}

		// Exposes physical slot storage. Access only slots that contain live T objects.
		T* data()
		{
			return m_values;
		}

		const T* data() const
		{
			return m_values;
		}

		Iterator begin()
		{
			return Iterator(this, 0);
		}

		Iterator end()
		{
			return Iterator(this, m_capacity);
		}

		ConstIterator begin() const
		{
			return ConstIterator(this, 0);
		}

		ConstIterator end() const
		{
			return ConstIterator(this, m_capacity);
		}

	private:
		static constexpr u32 NO_SLOT = ~0u;

		// Separate lookup tables from mutation tables at cache-line boundaries.
		static constexpr size_t ALIGNMENT = alignof(T) > EMBER_CACHE_LINE
		                                        ? alignof(T)
		                                        : size_t(EMBER_CACHE_LINE);

		struct Layout
		{
			size_t generations = 0;
			size_t ring = 0;
			size_t live = 0;
			size_t bytes = 0;
		};

		static u32 live_words(u32 capacity)
		{
			return (capacity + 63) / 64;
		}

		static Layout make_layout(u32 capacity)
		{
			const size_t line = EMBER_CACHE_LINE;
			Layout layout;

			layout.generations = align_up(size_t(capacity) * sizeof(T), line);
			layout.ring = align_up(layout.generations + size_t(capacity) * sizeof(u16), line);
			layout.live = align_up(layout.ring + size_t(capacity) * sizeof(u32), line);
			layout.bytes = layout.live + size_t(live_words(capacity)) * sizeof(u64);

			return layout;
		}

		// Bounds and generation check only. Callers supply issued handles, not fabricated bits.
		u32 find(Handle handle) const
		{
			const u32 index = handle.index();
			if (index >= m_capacity || m_generations[index] != handle.generation())
			{
				return NO_SLOT;
			}

			return index;
		}

		bool is_live(u32 index) const
		{
			return ((m_live[index / 64] >> (index % 64)) & 1) != 0;
		}

		// Invalidate the issued handle before running the destructor, which may resolve handles.
		void kill(u32 index)
		{
			const u32 next = m_generations[index] + 1u;
			m_generations[index] = (u16)(next > Handle::MAX_GENERATION ? 1u : next);
			m_live[index / 64] &= ~(u64(1) << (index % 64));
			m_values[index].~T();
		}

		u32 take_slot()
		{
			const u32 index = m_ring[m_ring_head];
			m_ring_head = m_ring_head + 1 == m_capacity ? 0 : m_ring_head + 1;
			--m_free;
			return index;
		}

		void give_slot(u32 index)
		{
			u32 tail = m_ring_head + m_free;
			if (tail >= m_capacity)
			{
				tail -= m_capacity;
			}

			m_ring[tail] = index;
			++m_free;
		}

		void reset_ring()
		{
			for (u32 index = 0; index < m_capacity; ++index)
			{
				m_ring[index] = index;
			}

			m_ring_head = 0;
			m_free = m_capacity;
		}

		memory::Allocator* m_allocator = nullptr;

		T* m_values = nullptr;
		u16* m_generations = nullptr;
		u32* m_ring = nullptr;
		u64* m_live = nullptr;
		size_t m_bytes = 0;

		u32 m_capacity = 0;
		u32 m_ring_head = 0;
		u32 m_free = 0;
		u32 m_retired = 0;
	};
}
