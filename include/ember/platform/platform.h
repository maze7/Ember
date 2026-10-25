#pragma once

// The platform independence layer. Sits over SDL3, with the SDL3 implementation confined to one
// source file so it is easy to later support other implementations if necessary.
//
// Everything here runs on main's fiber (Worker 0), which jobs.h pins to the thread that initialized
// it, because the desktop delivers its events to the thread that opened the window. Nothing here
// may be called from a job.
//
// A frame begins with [pump()], every pending event is drained and folded into the snapshot the
// frame then reads, with no callbacks. Windows are named handles from a pool.

#include <ember/containers/string.h>
#include <ember/core/types.h>
#include <ember/math/geometry.h>
#include <ember/platform/input.h>
#include <ember/platform/window.h>

namespace ember::platform
{
	struct PlatformDef
	{
		// Windows the pool holds. Games usually need only 1; tools may want more.
		u32 max_windows = 4;

		// Makes every window able to carry a Vulkan surface, which loads the Vulkan library at the
		// first window. Off for a build without graphics.
		bool vulkan = true;

		// Pads keep reporting while the window is not focused, so a player who alt-tabs out does
		// not drive a stuck stick.
		bool background_gamepads = true;
	};

	// What kind of pad sits in a slot, so prompts can show the right glyphs.
	enum class GamepadType : u8
	{
		Unknown,
		Standard,
		Xbox360,
		XboxOne,
		PlayStation3,
		PlayStation4,
		PlayStation5,
		SwitchPro,
		SwitchJoyConLeft,
		SwitchJoyConRight,
		SwitchJoyConPair,
		GameCube,
		Steam,

		Count
	};

	// Facts about a connected pad, from gamepad_info(). Not part of the snapshot, since they do
	// not change between frames.
	struct GamepadInfo
	{
		char name[64] = {};
		GamepadType type = GamepadType::Unknown;
		u16 vendor = 0;
		u16 product = 0;
		u16 product_version = 0;
	};

	// Initializes the platform layer, plumbs allocators and creates the window pool.
	bool initialize(const PlatformDef& def = {});

	// Destroys what is still open, pads and windows included, and shuts down the platform layer.
	void shutdown();

	// Drains every pending event into the snapshot the frame will read and returns true when the
	// OS asked the program to quit, which includes the last window being closed. Once per frame,
	// before the frame samples anything.
	bool pump(InputState& input);

	WindowHandle create_window(const WindowDef& def = {});
	void destroy_window(WindowHandle window);

	// The size of the window's back buffer. 0x0 for a minimized window.
	Extent2D window_size_in_pixels(WindowHandle window);

	// How much bigger the desktop wants content drawn in this window than at 100 percent, so a UI
	// can lay itself out in points and draw in pixels.
	f32 window_display_scale(WindowHandle window);

	void set_window_title(WindowHandle window, const char* title);

	// In points, like WindowDef.
	void set_window_size(WindowHandle window, u32 width, u32 height);

	// A caller says the mode it wants every frame; only a change reaches the desktop.
	void set_cursor_mode(WindowHandle window, CursorMode mode);

	// The shape the cursor takes until set again. A game that draws its own hides the cursor
	// through set_cursor_mode instead.
	void set_cursor(SystemCursor cursor);

	// Between start and stop the snapshot carries the text typed into the window and the desktop
	// may show its own composition UI. Off, keys are keys and text stays empty.
	void start_text_input(WindowHandle window);
	void stop_text_input(WindowHandle window);

	// Copies the clipboard into [out], through its allocator. False, logged, when the desktop
	// could not say.
	bool clipboard_text(String& out);
	void set_clipboard_text(const char* text);

	// False when no pad is connected in the slot.
	bool gamepad_info(u32 index, GamepadInfo& out);

	// Shakes the pad in a slot, intensities in [0, 1], for duration_ms. Nothing when the slot is
	// empty or the pad cannot.
	void gamepad_rumble(u32 index, f32 low, f32 high, u32 duration_ms);
}
