#pragma once

// Identifies the subsystem charged for an allocation. Heap and arena reports
// use these tags to show where memory goes. Unknown covers allocations whose
// caller did not provide a tag.

#include <ember/core/types.h>

#include <cstddef>

namespace ember::memory
{
	enum class Tag : u8
	{
		Unknown = 0,
		Engine,
		Graphics,
		Audio,
		Physics,
		Gameplay,
		Assets,
		Scripting,
		Network,
		Platform,
		Input,

		// Editor and diagnostic work, such as debug drawing and profiling overlays.
		// Heap allocations use development memory when it is available.
		// Arena allocations still come from the shared block pool.
		Tools,

		Count
	};

	inline constexpr size_t TAG_COUNT = (size_t)Tag::Count;
}
