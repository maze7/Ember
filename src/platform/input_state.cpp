#include <platform/input_state.h>

#include <ember/core/macros.h>

#include <cstring>

namespace ember::platform
{
	namespace
	{
		bool test_bit(const u64* words, u32 index)
		{
			return (words[index / 64] >> (index % 64) & 1) != 0;
		}

		void set_bit(u64* words, u32 index)
		{
			words[index / 64] |= u64(1) << (index % 64);
		}

		void clear_bit(u64* words, u32 index)
		{
			words[index / 64] &= ~(u64(1) << (index % 64));
		}

		// Synthesize releases when focus or a device is lost, since no release events may follow.
		void release_all(u64* down, u64* released, u32 words)
		{
			for (u32 word = 0; word < words; ++word)
			{
				released[word] |= down[word];
				down[word] = 0;
			}
		}
	}

	void fold_key(InputState& pending, Key key, bool down, u64 now_ns)
	{
		const u32 index = (u32)key;
		EMBER_ASSERT(index < KEY_COUNT);

		if (key == Key::Unknown || test_bit(pending.keys_down, index) == down)
		{
			return;
		}

		if (down)
		{
			set_bit(pending.keys_down, index);
			set_bit(pending.keys_pressed, index);
		}
		else
		{
			clear_bit(pending.keys_down, index);
			set_bit(pending.keys_released, index);
		}

		pending.keyboard_ns = now_ns;
	}

	void fold_text(InputState& pending, const char* text, u32 size)
	{
		if (size == 0 || pending.text_size + size > TEXT_CAPACITY)
		{
			return;
		}

		std::memcpy(pending.text + pending.text_size, text, size);
		pending.text_size += size;
	}

	void fold_mouse_button(InputState& pending, MouseButton button, bool down, WindowHandle window,
	    u64 now_ns)
	{
		const u32 bit = u32(1) << (u32)button;
		EMBER_ASSERT((u32)button < (u32)MouseButton::Count);

		if (button == MouseButton::None || ((pending.mouse_down & bit) != 0) == down)
		{
			return;
		}

		if (down)
		{
			pending.mouse_down |= bit;
			pending.mouse_pressed |= bit;
		}
		else
		{
			pending.mouse_down &= ~bit;
			pending.mouse_released |= bit;
		}

		pending.mouse_window = window;
		pending.mouse_ns = now_ns;
	}

	void fold_mouse_move(InputState& pending, f32 x, f32 y, f32 dx, f32 dy, WindowHandle window,
	    u64 now_ns)
	{
		if (pending.mouse_window != window)
		{
			pending.mouse_dx = 0.0f;
			pending.mouse_dy = 0.0f;
		}

		pending.mouse_x = x;
		pending.mouse_y = y;
		pending.mouse_dx += dx;
		pending.mouse_dy += dy;
		pending.mouse_window = window;
		pending.mouse_ns = now_ns;
	}

	void fold_mouse_wheel(InputState& pending, f32 x, f32 y, WindowHandle window, u64 now_ns)
	{
		pending.wheel_x += x;
		pending.wheel_y += y;
		pending.mouse_window = window;
		pending.mouse_ns = now_ns;
	}

	void fold_focus(InputState& pending, WindowHandle window, bool gained, u64 now_ns)
	{
		if (gained)
		{
			pending.focus = window;
			return;
		}

		if (pending.focus != window)
		{
			return;
		}

		release_all(pending.keys_down, pending.keys_released, KEY_WORDS);
		pending.mouse_released |= pending.mouse_down;
		pending.mouse_down = 0;
		pending.mouse_dx = 0.0f;
		pending.mouse_dy = 0.0f;
		pending.text_size = 0;
		pending.focus = WindowHandle{};
		pending.keyboard_ns = now_ns;
		pending.mouse_ns = now_ns;
	}

	void fold_close_request(InputState& pending, WindowHandle window)
	{
		pending.close_requested = window;
	}

	void fold_gamepad_connection(InputState& pending, u32 index, bool connected, u64 now_ns)
	{
		EMBER_ASSERT(index < MAX_GAMEPADS);
		Gamepad& gamepad = pending.gamepads[index];

		if (connected)
		{
			gamepad = Gamepad{};
			gamepad.connected = true;
			gamepad.last_input_ns = now_ns;
			return;
		}

		gamepad.released_bits |= gamepad.down_bits;
		gamepad.down_bits = 0;
		for (f32& axis : gamepad.axes)
		{
			axis = 0.0f;
		}

		gamepad.connected = false;
		gamepad.last_input_ns = now_ns;
	}

	void fold_gamepad_button(InputState& pending, u32 index, GamepadButton button, bool down,
	    u64 now_ns)
	{
		EMBER_ASSERT(index < MAX_GAMEPADS);
		EMBER_ASSERT((u32)button < (u32)GamepadButton::Count);
		Gamepad& gamepad = pending.gamepads[index];
		const u16 bit = (u16)(u32(1) << (u32)button);

		if (!gamepad.connected || ((gamepad.down_bits & bit) != 0) == down)
		{
			return;
		}

		if (down)
		{
			gamepad.down_bits |= bit;
			gamepad.pressed_bits |= bit;
		}
		else
		{
			gamepad.down_bits &= (u16)~bit;
			gamepad.released_bits |= bit;
		}

		gamepad.last_input_ns = now_ns;
	}

	void fold_gamepad_axis(InputState& pending, u32 index, GamepadAxis axis, f32 value, u64 now_ns)
	{
		EMBER_ASSERT(index < MAX_GAMEPADS);
		EMBER_ASSERT((u32)axis < (u32)GamepadAxis::Count);
		Gamepad& gamepad = pending.gamepads[index];

		if (!gamepad.connected || gamepad.axes[(u32)axis] == value)
		{
			return;
		}

		gamepad.axes[(u32)axis] = value;

		// A resting stick drifts. Only a deflection says the player is holding the pad.
		if (value >= GAMEPAD_NOISE || value <= -GAMEPAD_NOISE)
		{
			gamepad.last_input_ns = now_ns;
		}
	}

	void publish(InputState& pending, InputState& frame, u64 now_ns)
	{
		pending.time_ns = now_ns;
		frame = pending;

		for (u32 word = 0; word < KEY_WORDS; ++word)
		{
			pending.keys_pressed[word] = 0;
			pending.keys_released[word] = 0;
		}

		pending.mouse_pressed = 0;
		pending.mouse_released = 0;
		pending.mouse_dx = 0.0f;
		pending.mouse_dy = 0.0f;
		pending.wheel_x = 0.0f;
		pending.wheel_y = 0.0f;
		pending.close_requested = WindowHandle{};
		pending.text_size = 0;

		for (Gamepad& gamepad : pending.gamepads)
		{
			gamepad.pressed_bits = 0;
			gamepad.released_bits = 0;
		}
	}
}
