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
			// Preallocate storage space
			m_slots.resize(PoolSize);
			build_freelist(0);
		}

		template<class... Args>
		Handle<HandleType> emplace(Args &&... args) {
			// pull next available slot from the freelist
			u32 slot = m_slots[0].next;
			m_slots[0].next = m_slots[slot].next;

			// if the freelist is empty, the slot will be 0 as the freelist is a circularly linked list.
			// in this situation, we need to grow the slot array to accommodate more space
			if (slot == 0) {
				slot = m_slots.size();
				m_slots.resize(m_slots.size() * 2);
				build_freelist(slot);
				m_slots[0].next = slot + 1;
			}

			// Ensure that slot is within bounds
			EMBER_ASSERT(slot < m_slots.size());

			// construct in place & return handle to created element
			m_slots[slot].gen++;
			m_slots[slot].active = true;
			m_slots[slot].data.emplace(std::forward<Args>(args)...);
			return {slot, m_slots[slot].gen};
		}

		void erase(Handle<HandleType> handle) {
			EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);

			// call destructor on object and update generation
			m_slots[handle.slot].data.reset();
			m_slots[handle.slot].gen++;
			m_slots[handle.slot].active = false;

			// update the freelist
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

	private:
		struct Slot {
			Slot() : gen(0), next(0), active(false) {}

			// Define the destructor to manually destroy the data if active
			~Slot() {
				if (active) {
					data.reset(); // Explicitly reset the optional to destroy the contained object
				}
			}

			// Move constructor and move assignment
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
				u32 next;
			};

			Slot(const Slot&) = delete;
			Slot& operator=(const Slot&) = delete;
		};

		void build_freelist(u32 start_slot) {
			// initialize the freelist elements starting from 'start_slot'
			for (u32 i = start_slot; i < m_slots.size() - 1; ++i) {
				m_slots[i].next = i + 1;
			}

			// the last slot should always point back to the head of the freelist, in this implementation slot 0
			// is always reserved as the head, so we can point the last slot in the freelist to slot 0.
			m_slots[m_slots.size() - 1].next = 0;
		}

		std::vector<Slot> m_slots;
	};
}
