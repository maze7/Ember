#pragma once

// Dynamic arrays backed by an engine allocator and memory tag.
// Defaults to the heap under Tag::Unknown. Keep the allocator alive until the vector is destroyed,
// and destroy the vector before reclaiming arena storage.

#include <ember/memory/allocator.h>

#include <vector>

namespace ember
{
	template <typename T>
	using Vector = std::vector<T, memory::TagAllocator<T>>;
}
