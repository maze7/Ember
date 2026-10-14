#pragma once

#include <vector>

namespace Ember
{
	// Ember uses polymorphic-allocator (PMR) containers, so the allocator is chosen at runtime by
	// whoever owns the data rather than baked into the type. These aliases hide the std::pmr::
	// prefix for convenience and readability, and also allow for a later refactor away from STL
	// types without touching the whole codebase.
	template <typename T>
	using Vector = std::pmr::vector<T>;
}
