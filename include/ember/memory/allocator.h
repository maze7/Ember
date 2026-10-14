#pragma once

// The allocator interface every container and system allocates through. It is
// std::pmr::memory_resource under the hood. The engine's heaps, arenas and pools implement it as
// Allocator, so we can later move away from PMR if necessary.

#include <memory_resource>

namespace ember
{
	using Allocator = std::pmr::memory_resource;
}
