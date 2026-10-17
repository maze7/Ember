#pragma once

// A pool gives a value a fixed slot and a Handle to find it by. Windows, GPU resources and scene
// objects live in pools, and the rest of the engine holds handles to them rather than pointers. A
// handle packs the slot's index with the generation the slot had when the value was made. Freeing
// the slot advances its generation, so a handle kept past its value's life fails get() instead of
// reading whatever took the slot. Slots never move, which keeps a pointer from get() good until
// its slot is freed, and lets the index address tables kept beside the pool: a bindless descriptor
// array, or the cold half of a resource that the hot loop never touches.
//
// Storage is one allocation, taken at initialize() through an Allocator under a Tag and never
// grown. Capacity is a budget: a full pool returns the null handle and the caller decides what
// that means. Free slots wait their turn in a FIFO ring, so a freed slot comes back only after
// every other free slot has. A slot's generation wraps after 4095 frees, and the ring is what
// keeps a stale handle from meeting its slot's next generation any sooner than it must.
//
// Threading. emplace, erase, retire, release and clear mutate the pool and need the caller's lock.
// get() needs none: it reads one slot's generation, which changes only when that slot is retired,
// and a retire already has to come after the slot's last reader. Iteration reads the live bits
// that mutations write, so it takes the lock too.
//
// Costs. get() is a bounds check and one compare. emplace() and erase() are a ring step and a bit
// flip around the value's constructor or destructor. Iteration visits every slot, live or not.

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
	// Tag names the handle type, so Pool<SdlWindow, Window> hands out Handle<Window> for values
	// of type SdlWindow. It defaults to T for the common case where the value is its own name.
	template <typename T, typename Tag = T>
	class Pool final
	{
		static_assert(std::is_nothrow_destructible_v<T>, "pool values are destroyed by erase");

	public:
		using Handle = ember::Handle<Tag>;

		// A handle's index is twenty bits, so a pool holds at most 1,048,576 slots.
		static constexpr u32 MAX_CAPACITY = Handle::MAX_INDEX + 1;

		// Walks the live slots in index order. handle() names the slot under the iterator, for
		// code that needs to pass the value on.
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

		// Takes [capacity] slots, in [1, MAX_CAPACITY], through [allocator] under [tag], and
		// constructs no values. Runs once, before any emplace. The allocator must outlive the pool.
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

			// Generation zero is the null handle's, so no slot ever carries it.
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

		// Destroys every live value and returns the storage. Every handle given out is garbage
		// afterwards. The pool can be initialized again.
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

		// Constructs a value in the oldest free slot and returns its handle, or the null handle
		// when every slot is live or retired. The handle carries the slot's current generation,
		// which every handle the slot gave out before has already stopped matching.
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

		// Destroys the value and puts its slot back on the ring. False for a null or stale handle,
		// which destroys nothing.
		bool erase(Handle handle)
		{
			if (!retire(handle))
			{
				return false;
			}

			release(handle.index());
			return true;
		}

		// Destroys the value and kills the handle now, but keeps the slot off the ring until
		// release(). For a resource whose index is still named by work in flight, such as a GPU
		// frame reading the descriptor at that index: the value dies with the handle, the slot
		// waits for the fence. False for a null or stale handle.
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

		// Puts a retired slot back on the ring. The caller has proven that nothing in flight
		// names the index any more; nothing here can check that.
		void release(u32 index)
		{
			EMBER_ASSERT(index < m_capacity);
			EMBER_ASSERT_MSG(!is_live(index), "release of a live slot");
			EMBER_ASSERT_MSG(m_retired != 0, "release without a retire");

			--m_retired;
			give_slot(index);
		}

		// The value a handle names, or null for the null handle, a stale handle or an index past
		// the capacity. The pointer is good until the slot is erased or retired.
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

		// Destroys every live value and frees every slot, retired ones included, keeping the
		// storage and the generations, so every handle given out before stays stale. For a level
		// unload, once nothing in flight names a slot.
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

		// Every slot's value by index, live or not, for a table beside the pool that addresses
		// slots by index. A dead slot holds whatever its last value left behind, so only a
		// trivially destructible T should be read this way.
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

		// The values and the generations are read on the hot path and the ring and the live bits
		// are written on the mutating one, so each table starts on its own cache line.
		static constexpr size_t ALIGNMENT = alignof(T) > EMBER_CACHE_LINE
		                                        ? alignof(T)
		                                        : size_t(EMBER_CACHE_LINE);

		// Byte offsets of the tables that follow the values in the one allocation.
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

		// The slot a handle names, or NO_SLOT for the null handle, a stale handle or an index past
		// the capacity. A free slot's generation matches no handle it ever gave out.
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

		// Ends the slot's current life: the generation moves on, skipping zero, before the value's
		// destructor runs, so nothing the destructor does can find the value through its handle.
		void kill(u32 index)
		{
			const u32 next = m_generations[index] + 1u;
			m_generations[index] = (u16)(next > Handle::MAX_GENERATION ? 1u : next);
			m_live[index / 64] &= ~(u64(1) << (index % 64));
			m_values[index].~T();
		}

		// Pops the oldest free slot. The caller has checked that there is one.
		u32 take_slot()
		{
			const u32 index = m_ring[m_ring_head];
			m_ring_head = m_ring_head + 1 == m_capacity ? 0 : m_ring_head + 1;
			--m_free;
			return index;
		}

		// Pushes a slot behind every slot already waiting. The ring holds one entry per slot, so
		// it cannot overflow.
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

		// Every slot free, in index order, so a fresh pool hands out 0, 1, 2 and so on.
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

		// One allocation: the values first, then each table on its own cache line.
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
