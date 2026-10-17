#pragma once

// An OS window. The platform layer owns every OS Window and hands out a Handle, which stops
// resolving once the window is destroyed. This header holds only what a Def or an input snapshot
// has to say about a window, so that input.h can name one without the platform's whole surface
// behind it. The functions that make and change windows are in platform.h

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

		// Asks for a back buffer at the display's full pixel density. Without it a scaled display
		// gives a window fewer pixels than points.
		HighPixelDensity = 1 << 4,
	};

	EMBER_ENUM_BITWISE_OPS(WindowFlags);

	// How a window is made. The size is in points, the units the desktop lays windows out in, and
	// is clamped to the dispaly. The pixels behind a window come from window_size_in_pixels() once
	// it exists. The title is borrowed for the call.
	struct WindowDef
	{
		const char* title = "Ember";
		u32 width = 1280;
		u32 height = 720;
		WindowFlags flags = WindowFlags::Resizable | WindowFlags::HighPixelDensity;
	};

	// What the cursor does over a window. Relative hides it, keeps it in the window and reports
	// motion as deltas only, for mouse look.
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
