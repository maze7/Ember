#pragma once

// Event accumulation shared by platform backends. Backends supply engine window handles and
// the pump's timestamp. Folds update fixed storage; publish() copies it into the frame snapshot
// and clears transient state.

#include <ember/core/types.h>
#include <ember/input/input.h>

namespace ember::platform
{
	// Ignores key repeats so held keys do not create extra press edges.
	void fold_key(InputState& pending, Key key, bool down, u64 now_ns);

	// UTF-8 typed, appended to the frame's text or dropped whole when it does not fit.
	void fold_text(InputState& pending, const char* text, u32 size);

	void fold_mouse_button(InputState& pending, MouseButton button, bool down, WindowHandle window,
	    u64 now_ns);

	// Position is in window pixels; motion is in points. Changing windows discards prior motion.
	void fold_mouse_move(InputState& pending, f32 x, f32 y, f32 dx, f32 dy, WindowHandle window,
	    u64 now_ns);

	void fold_mouse_wheel(InputState& pending, f32 x, f32 y, WindowHandle window, u64 now_ns);

	// Losing focus releases all keys and mouse buttons; later releases may go to another app.
	void fold_focus(InputState& pending, WindowHandle window, bool gained, u64 now_ns);

	void fold_close_request(InputState& pending, WindowHandle window);

	// A pad arrived in or left slot [index]. Leaving releases what it held.
	void fold_gamepad_connection(InputState& pending, u32 index, bool connected, u64 now_ns);

	void fold_gamepad_button(InputState& pending, u32 index, GamepadButton button, bool down,
	    u64 now_ns);

	// [value] already normalised: a stick axis in [-1, 1], a trigger in [0, 1].
	void fold_gamepad_axis(InputState& pending, u32 index, GamepadAxis axis, f32 value, u64 now_ns);

	// Copies pending state to the frame and stamps now_ns. Clears edges, motion, wheel, text
	// and close requests; held buttons and connections carry into the next pump.
	void publish(InputState& pending, InputState& frame, u64 now_ns);
}
