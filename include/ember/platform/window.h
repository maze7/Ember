#pragma once

// Window types shared by platform configuration and input snapshots. The platform owns each
// OS window; its handle stops resolving after destruction. Window operations are in platform.h.

#include <ember/core/enum.h>
#include <ember/core/handle.h>
#include <ember/core/types.h>

namespace ember
{
	struct Window;
	using WindowHandle = Handle<Window>;

	enum class WindowFlags : u8
	{
		None = 0,
		Resizable = 1 << 0,
		Fullscreen = 1 << 1,
		Hidden = 1 << 2,
		Borderless = 1 << 3,

		// Requests the display's full pixel density on scaled displays.
		HighPixelDensity = 1 << 4,
	};

	EMBER_ENUM_BITWISE_OPS(WindowFlags);

	// Size is in desktop points and clamped to the display's usable area. Query
	// window_size_in_pixels() for render dimensions. The title is borrowed only during creation.
	struct WindowDef
	{
		const char* title = "Ember";
		u32 width = 1280;
		u32 height = 720;
		WindowFlags flags = WindowFlags::Resizable | WindowFlags::HighPixelDensity;
	};

	// Relative mode hides and captures the cursor, reporting deltas for mouse look.
	enum class CursorMode : u8
	{
		Normal,
		Hidden,
		Relative,
	};

	// The desktop's cursor shapes, in the order SDL names them.
	enum class SystemCursor : u8
	{
		Default,
		Text,
		Wait,
		Crosshair,
		Progress,
		ResizeNWSE,
		ResizeNESW,
		ResizeEW,
		ResizeNS,
		Move,
		NotAllowed,
		Pointer,
		ResizeNW,
		ResizeN,
		ResizeNE,
		ResizeE,
		ResizeSE,
		ResizeS,
		ResizeSW,
		ResizeW,

		Count
	};
}
