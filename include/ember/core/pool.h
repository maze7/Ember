#pragma once

#include <vector>
#include <cstddef>
#include <type_traits>
#include <utility>

#include "core/common.h"
#include "core/handle.h"

namespace Ember {

template<class T, class HandleType = T, int InitialPoolSize = 256>
class Pool {
public:
    Pool() {
        m_slots.reserve(InitialPoolSize);
        m_slots.emplace_back(); // Slot 0 is reserved as the freelist head
        build_freelist(1, InitialPoolSize);
    }

    template<class... Args>
    Handle<HandleType> emplace(Args&&... args) {
        if (m_freelist_head == 0) {
            grow_pool();
        }

        u32 slot = m_freelist_head;
        m_freelist_head = m_slots[slot].next;

        Slot& slot_ref = m_slots[slot];
        slot_ref.gen++;
        slot_ref.construct(std::forward<Args>(args)...);

        return {slot, slot_ref.gen};
    }

    void erase(Handle<HandleType> handle) {
        EMBER_ASSERT(handle.slot < m_slots.size() && "Invalid slot index!");
        Slot& slot_ref = m_slots[handle.slot];
        EMBER_ASSERT(handle.gen == slot_ref.gen && "Handle generation mismatch!");

        if (handle.gen == slot_ref.gen) {
            slot_ref.dispose();
            slot_ref.next = m_freelist_head;
            m_freelist_head = handle.slot;
        }
    }

    T* get(Handle<HandleType> handle) {
        if (is_valid(handle)) {
            return m_slots[handle.slot].get();
        }
        return nullptr;
    }

    const T* get(Handle<HandleType> handle) const {
        if (is_valid(handle)) {
            return m_slots[handle.slot].get();
        }
        return nullptr;
    }

    bool is_valid(Handle<HandleType> handle) const {
        return handle.slot < m_slots.size() &&
               m_slots[handle.slot].gen == handle.gen &&
               m_slots[handle.slot].active;
    }

    class iterator {
    public:
        struct reference_proxy {
            reference_proxy(Pool* pool, size_t index)
                : m_pool(pool), m_index(index) {}

            operator T&() const {
                return *m_pool->m_slots[m_index].get();
            }

            Handle<HandleType> handle() const {
                return {static_cast<u32>(m_index), m_pool->m_slots[m_index].gen};
            }

        private:
            Pool* m_pool;
            size_t m_index;
        };

        iterator(Pool* pool, size_t index)
            : m_pool(pool), m_index(index) {
            advance_past_inactive();
        }

        reference_proxy operator*() const {
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
        return iterator(this, 1); // Start from slot 1
    }

    iterator end() {
        return iterator(this, m_slots.size());
    }

private:
    struct Slot {
        u32 gen = 0;
        bool active = false;

        union {
            typename std::aligned_storage<sizeof(T), alignof(T)>::type data;
            u32 next;
        };

        Slot() : next(0), active(false) {}
        ~Slot() { dispose(); }

        Slot(Slot&& other) noexcept : gen(other.gen), active(other.active) {
            if (active) {
                construct(std::move(*other.get()));
            } else {
                next = other.next;
            }
            other.dispose();
        }

        Slot& operator=(Slot&& other) noexcept {
            if (this != &other) {
                dispose();
                gen = other.gen;
                active = other.active;
                if (active) {
                    construct(std::move(*other.get()));
                } else {
                    next = other.next;
                }
                other.dispose();
            }
            return *this;
        }

        void dispose() {
            if (active) {
                reinterpret_cast<T*>(&data)->~T();
                active = false;
            }
        }

        T* get() {
            return active ? reinterpret_cast<T*>(&data) : nullptr;
        }

        const T* get() const {
            return active ? reinterpret_cast<const T*>(&data) : nullptr;
        }

        template<class... Args>
        void construct(Args&&... args) {
            new (&data) T(std::forward<Args>(args)...);
            active = true;
        }

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
    };

    void build_freelist(u32 start, u32 end) {
        for (u32 i = start; i < end - 1; ++i) {
            m_slots[i].next = i + 1;
        }
        m_slots[end - 1].next = 0; // End of the freelist
    }

    void grow_pool() {
        u32 old_size = m_slots.size();
        u32 new_size = old_size * 2;
        m_slots.resize(new_size);
        build_freelist(old_size, new_size);
        m_freelist_head = old_size;
    }

    std::vector<Slot> m_slots;
    u32 m_freelist_head = 0;
};

} // namespace Ember
