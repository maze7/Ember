#pragma once

#include <memory>
#include <vector>

#include "platform.h"

namespace Ember
{
    template <class T>
    struct Handle
    {
        u32 slot;
        u32 gen;
    };

    template <class T, class HandleType = T, int Size = 1024>
    class Pool
    {
    public:
        Pool() {
            // preallocate storage
            m_slots.resize(Size);
            build_freelist(0);
        }

        template <class... Args>
        Handle<HandleType> emplace(Args&&... args) {
            // pull next available slot from the freelist
            u32 slot = m_slots[0].next;
            m_slots[0].next = m_slots[slot].next;

            // if the freelist is empty, the slot will be 0, as the freelist is a circularly linked list
            // in this situation, we need to grow the slot array to accompany more space
            if (slot == 0) {
                slot = m_slots.size();
                m_slots.resize(m_slots.size() * 2);
                build_freelist(slot);
                m_slots[0].next = slot + 1;
            }

            // construct in place & return handle to created element
            //std::construct_at(&(m_slots[slot].data), std::forward<Args>(args)...);
            new (&(m_slots[slot].data)) T(std::forward<Args>(args)...);
            return { slot, m_slots[slot].gen };
        }

        void erase(Handle<HandleType> handle) {
            EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);

            // call destructor on object and update generation
            m_slots[handle.slot].data.~T();
            m_slots[handle.slot].gen++;

            // update the freelist
            m_slots[handle.slot].next = m_slots[0].next;
            m_slots[0].next = handle.slot;
        }

        T* get(Handle<HandleType> handle) {
            EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);
            return &(m_slots[handle.slot].data);
        }

        const T* get(Handle<HandleType> handle) const {
            EMBER_ASSERT(handle.gen == m_slots[handle.slot].gen);
            return &(m_slots[handle.slot].data);
        }

    private:
        void build_freelist(u32 start_slot) {
            // Initialize the freelist elements starting from 'start_slot'
            for (u32 i = start_slot; i < m_slots.size() - 1; ++i) {
                m_slots[i].next = i + 1;
            }

            // The last slot should point back to the head of the freelist, in this implementation slot 0 is always reserved as the head.
            m_slots[m_slots.size() - 1].next = 0;
        }

    private:
        struct Slot
        {
            Slot(): gen(-1), next(-1) {}

            u32 gen;
            union {
                T 	data;
                u32 next;
            };
        };

        // TODO: I hate using a std::vector, we'll need to use something more memory efficient like some form of TLSF
        // Allocator. For now I just want to get something on the screen though so will improve this later.
        std::vector<Slot> 		m_slots;
        u32 					m_head = 0;
        u32 					m_tail = 0;
    };
}