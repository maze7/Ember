#pragma once

// Plain sizes, offsets and shapes shared by the rest of the engine.

#include <ember/core/types.h>

namespace ember
{
	struct Extent2D
	{
		u32 width = 0;
		u32 height = 0;
	};

	struct Extent3D
	{
		u32 width = 1;
		u32 height = 1;
		u32 depth = 1;
	};

	struct Offset3D
	{
		i32 x = 0;
		i32 y = 0;
		i32 z = 0;
	};

	struct Rect
	{
		i32 x = 0;
		i32 y = 0;
		u32 width = 0;
		u32 height = 0;
	};

	// Depth defaults to the full [0, 1] range; reverse-Z views set it the other way round.
	struct Viewport
	{
		f32 x = 0.0f;
		f32 y = 0.0f;
		f32 width = 0.0f;
		f32 height = 0.0f;
		f32 min_depth = 0.0f;
		f32 max_depth = 1.0f;
	};

	// The GPU backend passes these straight to Vulkan, so their layout is held here.
	static_assert(sizeof(Extent2D) == 8 && sizeof(Extent3D) == 12 && sizeof(Offset3D) == 12);
	static_assert(sizeof(Rect) == 16 && sizeof(Viewport) == 24);
}
