#pragma once

// The engine's half of the event pump, shared by every backend and tested without one. The backend
// translates each OS event into one fold on the pending snapshot, with the window and the pump's
// time already in engine terms, and publish() hands the frame what the devices have done since the
// last pump. A fold is a few bit operations; none of them allocate or fail.

#include <ember/core/types.h>
#include <ember/platform/input.h>

namespace ember::platform
{
	// A key's level changed. Repeats the OS makes for a held key are not changes and are not
	// folded.
	void fold_key(InputState& pending, Key key, bool down, u64 now_ns);

	// UTF-8 typed, appended to the frame's text or dropped whole when it does not fit.
	void fold_text(InputState& pending, const char* text, u32 size);

	void fold_mouse_button(InputState& pending, MouseButton button, bool down, WindowHandle window,
	    u64 now_ns);

	// The pointer at (x, y) pixels of [window], having moved (dx, dy) points. Motion from another
	// window is not added to this one's.
	void fold_mouse_move(InputState& pending, f32 x, f32 y, f32 dx, f32 dy, WindowHandle window,
	    u64 now_ns);

	void fold_mouse_wheel(InputState& pending, f32 x, f32 y, WindowHandle window, u64 now_ns);

	// Keyboard focus came to or left [window]. Losing it releases every key and button, since
	// the desktop will not tell us when they come up elsewhere.
	void fold_focus(InputState& pending, WindowHandle window, bool gained, u64 now_ns);

	void fold_close_request(InputState& pending, WindowHandle window);

	// A pad arrived in or left slot [index]. Leaving releases what it held.
	void fold_gamepad_connection(InputState& pending, u32 index, bool connected, u64 now_ns);

	void fold_gamepad_button(InputState& pending, u32 index, GamepadButton button, bool down,
	    u64 now_ns);

	// [value] already normalised: a stick axis in [-1, 1], a trigger in [0, 1].
	void fold_gamepad_axis(InputState& pending, u32 index, GamepadAxis axis, f32 value, u64 now_ns);

	// Copies the pending snapshot into the frame's, stamped with [now_ns], and clears what was
	// this frame's alone: the edges, the motion, the wheel, the text and the close request.
	// Levels and connections carry on.
	void publish(InputState& pending, InputState& frame, u64 now_ns);
}
