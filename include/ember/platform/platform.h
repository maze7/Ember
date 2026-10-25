#pragma once

// Desktop windows and input, backed by SDL3. Call only from main's fiber on worker 0:
// desktop events belong to the thread that created the windows. pump() drains events into a
// snapshot once per frame, before the game reads input.

#include <ember/containers/string.h>
#include <ember/core/types.h>
#include <ember/math/geometry.h>
#include <ember/platform/input.h>
#include <ember/platform/window.h>

namespace ember::platform
{
	struct PlatformDef
	{
		// Fixed capacity; opening windows does not grow the pool.
		u32 max_windows = 4;

		// Enables Vulkan surfaces for every window. The first window loads the Vulkan library.
		bool vulkan = true;

		// Continue receiving gamepad events while no window is focused.
		bool background_gamepads = true;
	};

	// Identifies the button glyphs to use for a connected gamepad.
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

	// Metadata for the gamepad currently occupying a slot. Refresh after a connection change.
	struct GamepadInfo
	{
		char name[64] = {};
		GamepadType type = GamepadType::Unknown;
		u16 vendor = 0;
		u16 product = 0;
		u16 product_version = 0;
	};

	// Requires initialized memory and jobs. Returns false if SDL setup fails.
	bool initialize(const PlatformDef& def = {});

	// Destroys remaining windows, cursors and gamepads before shutting down SDL.
	void shutdown();

	// Publishes input and returns true on an OS quit request, including the last window closing.
	bool pump(InputState& input);

	WindowHandle create_window(const WindowDef& def = {});
	void destroy_window(WindowHandle window);

	// The size of the window's back buffer. 0x0 for a minimized window.
	Extent2D window_size_in_pixels(WindowHandle window);

	// Desktop content scale for laying out UI in points and drawing it in pixels.
	f32 window_display_scale(WindowHandle window);

	void set_window_title(WindowHandle window, const char* title);

	// In points, like WindowDef.
	void set_window_size(WindowHandle window, u32 width, u32 height);

	// May be called every frame; only changes reach the desktop.
	void set_cursor_mode(WindowHandle window, CursorMode mode);

	// Sets the shared system cursor shape. Use set_cursor_mode() to hide it.
	void set_cursor(SystemCursor cursor);

	// Enables UTF-8 text in the input snapshot and the desktop's composition UI. Key state
	// remains available whether text input is enabled or not.
	void start_text_input(WindowHandle window);
	void stop_text_input(WindowHandle window);

	// Copies clipboard text through out's allocator. Logs and returns false on failure.
	bool clipboard_text(String& out);
	void set_clipboard_text(const char* text);

	// False when no pad is connected in the slot.
	bool gamepad_info(u32 index, GamepadInfo& out);

	// Intensities are in [0, 1]. Does nothing for empty slots or gamepads without rumble.
	void gamepad_rumble(u32 index, f32 low, f32 high, u32 duration_ms);
}
