#pragma once

// The engine's dynamic array: std::vector over a TagAllocator, so that every vector carries the
// allocator it draws from and the tag it is charged to, by value. Vector<T> v(Tag::Audio) lives
// on the heap under Audio; Vector<T> v(&arena) lives in the arena and dies with its lifetime;
// Vector<T> v with nothing said lives on the heap under Unknown, where the tracker will show it.
//
// The alias is the seam. Code says Vector, never std::vector, so that the day the engine owns
// its container the spelling at every call site stays the same.

#include <ember/memory/allocator.h>

#include <vector>

namespace ember
{
	template <typename T>
	using Vector = std::vector<T, memory::TagAllocator<T>>;
}
