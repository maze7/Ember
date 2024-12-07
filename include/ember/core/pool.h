#pragma once

#include <optional>
#include <vector>

#include "core/common.h"
#include "core/handle.h"

namespace Ember
{
	template<class T, class HandleType = T, int PoolSize = 1024>
	class Pool
	{
	public:
		Pool() {
			m_slots.resize(PoolSize);
			build_freelist(0);
		}

		template<class... Args>
		Handle<HandleType> emplace(Args &&... args) {
			u32 slot = m_slots[0].next;
			m_slots[0].next = m_slots[slot].next;

			if (slot == 0) {
				slot = (u32)m_slots.size();
				m_slots.resize(m_slots.size() * 2);
				build_freelist(slot);
				m_slots[0].next = slot + 1;
			}

			EMBER_ASSERT(slot < m_slots.size());

			m_slots[slot].gen++;
			m_slots[slot].active = true;
			m_slots[slot].data.emplace(std::forward<Args>(args)...);
			return {slot, m_slots[slot].gen};
		}

		void erase(Handle<HandleType> handle) {
			EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);

			m_slots[handle.slot].data.reset();
			m_slots[handle.slot].gen++;
			m_slots[handle.slot].active = false;

			m_slots[handle.slot].next = m_slots[0].next;
			m_slots[0].next = handle.slot;
		}

		T *get(Handle<HandleType> handle) {
			EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);
			return &(m_slots[handle.slot].data.value());
		}

		const T *get(Handle<HandleType> handle) const {
			EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);
			return &(m_slots[handle.slot].data.value());
		}

		// Custom iterator classes
		class iterator {
		public:
			// The proxy object returned by operator*
			class reference_proxy {
			public:
				reference_proxy(Pool *pool, size_t index)
					: m_pool(pool), m_index(index) {}

				// Access the underlying T
				operator T&() const {
					return m_pool->m_slots[m_index].data.value();
				}

				// Provide a method to obtain the handle
				Handle<HandleType> handle() const {
					return Handle<HandleType>((u32)m_index, m_pool->m_slots[m_index].gen);
				}

				// If you want implicit conversion to Handle<HandleType>
				// so that destroy_shader(proxy) works directly:
				operator Handle<HandleType>() const {
					return handle();
				}

			private:
				Pool* m_pool;
				size_t m_index;
			};

			using difference_type = std::ptrdiff_t;
			using value_type = T;
			using pointer = T*;
			using reference = reference_proxy;
			using iterator_category = std::forward_iterator_tag;

			iterator(Pool* pool, size_t index)
				: m_pool(pool), m_index(index) {
				advance_past_inactive();
			}

			reference operator*() const {
				return reference_proxy(m_pool, m_index);
			}

			iterator& operator++() {
				++m_index;
				advance_past_inactive();
				return *this;
			}

			iterator operator++(int) {
				iterator tmp = *this;
				++(*this);
				return tmp;
			}

			bool operator==(const iterator& other) const {
				return m_pool == other.m_pool && m_index == other.m_index;
			}

			bool operator!=(const iterator& other) const {
				return !(*this == other);
			}

		private:
			void advance_past_inactive() {
				while (m_index < m_pool->m_slots.size() && !m_pool->m_slots[m_index].active) {
					++m_index;
				}
			}

			Pool* m_pool;
			size_t m_index;
		};

		iterator begin() {
			return iterator(this, 1); // start from slot 1, slot 0 is the freelist head
		}

		iterator end() {
			return iterator(this, m_slots.size());
		}

	private:
		struct Slot {
			Slot() : gen(0), next(0), active(false) {}

			~Slot() {
				if (active) {
					data.reset();
				}
			}

			Slot(Slot&& other) noexcept : gen(other.gen), active(other.active), data(std::move(other.data)) {
				other.active = false;
			}

			Slot& operator=(Slot&& other) noexcept {
				if (this != &other) {
					if (active) {
						data.reset();
					}
					gen = other.gen;
					active = other.active;
					data = std::move(other.data);
					other.active = false;
				}
				return *this;
			}

			u32 gen;
			bool active = false;
			union {
				std::optional<T> data;
				u32 next; // used when inactive
			};

			Slot(const Slot&) = delete;
			Slot& operator=(const Slot&) = delete;
		};

		void build_freelist(u32 start_slot) {
			for (u32 i = start_slot; i < m_slots.size() - 1; ++i) {
				m_slots[i].next = i + 1;
			}
			m_slots[m_slots.size() - 1].next = 0;
		}

		std::vector<Slot> m_slots;
	};
}