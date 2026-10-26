#pragma once

// Input snapshots copied into each frame record. Button edges accumulate from events, so a
// press and release within one frame both survive. The next pump clears edges, motion and text.
//
// Stick axes use [-1, 1], triggers use [0, 1], mouse position uses window pixels and motion uses
// desktop points. Timestamps use now_ns() at the pump; individual events are not timestamped.
// Text is UTF-8, and input beyond the fixed text capacity is dropped.

#include <ember/core/types.h>
#include <ember/math/scalar.h>
#include <ember/platform/window.h>

namespace ember
{
	// Keys by position on the keyboard, not by what they print: these are the USB HID usage ids.
	enum class Key : u8
	{
		Unknown = 0,
		A = 4,
		B = 5,
		C = 6,
		D = 7,
		E = 8,
		F = 9,
		G = 10,
		H = 11,
		I = 12,
		J = 13,
		K = 14,
		L = 15,
		M = 16,
		N = 17,
		O = 18,
		P = 19,
		Q = 20,
		R = 21,
		S = 22,
		T = 23,
		U = 24,
		V = 25,
		W = 26,
		X = 27,
		Y = 28,
		Z = 29,
		D1 = 30,
		D2 = 31,
		D3 = 32,
		D4 = 33,
		D5 = 34,
		D6 = 35,
		D7 = 36,
		D8 = 37,
		D9 = 38,
		D0 = 39,
		Enter = 40,
		Escape = 41,
		Backspace = 42,
		Tab = 43,
		Space = 44,
		Minus = 45,
		Equals = 46,
		LeftBracket = 47,
		RightBracket = 48,
		Backslash = 49,
		Semicolon = 51,
		Apostrophe = 52,
		Tilde = 53,
		Comma = 54,
		Period = 55,
		Slash = 56,
		Capslock = 57,
		F1 = 58,
		F2 = 59,
		F3 = 60,
		F4 = 61,
		F5 = 62,
		F6 = 63,
		F7 = 64,
		F8 = 65,
		F9 = 66,
		F10 = 67,
		F11 = 68,
		F12 = 69,
		PrintScreen = 70,
		ScrollLock = 71,
		Pause = 72,
		Insert = 73,
		Home = 74,
		PageUp = 75,
		Delete = 76,
		End = 77,
		PageDown = 78,
		Right = 79,
		Left = 80,
		Down = 81,
		Up = 82,
		Numlock = 83,
		KeypadDivide = 84,
		KeypadMultiply = 85,
		KeypadMinus = 86,
		KeypadPlus = 87,
		KeypadEnter = 88,
		Keypad1 = 89,
		Keypad2 = 90,
		Keypad3 = 91,
		Keypad4 = 92,
		Keypad5 = 93,
		Keypad6 = 94,
		Keypad7 = 95,
		Keypad8 = 96,
		Keypad9 = 97,
		Keypad0 = 98,
		KeypadPeriod = 99,
		Application = 101,
		KeypadEquals = 103,
		F13 = 104,
		F14 = 105,
		F15 = 106,
		F16 = 107,
		F17 = 108,
		F18 = 109,
		F19 = 110,
		F20 = 111,
		F21 = 112,
		F22 = 113,
		F23 = 114,
		F24 = 115,
		Execute = 116,
		Help = 117,
		Menu = 118,
		Select = 119,
		Stop = 120,
		Redo = 121,
		Undo = 122,
		Cut = 123,
		Copy = 124,
		Paste = 125,
		Find = 126,
		Mute = 127,
		VolumeUp = 128,
		VolumeDown = 129,
		KeypadComma = 133,
		AltErase = 153,
		SysReq = 154,
		Cancel = 155,
		Clear = 156,
		Prior = 157,
		Enter2 = 158,
		Separator = 159,
		Out = 160,
		Oper = 161,
		ClearAgain = 162,
		Keypad00 = 176,
		Keypad000 = 177,
		KeypadLeftParen = 182,
		KeypadRightParen = 183,
		KeypadLeftBrace = 184,
		KeypadRightBrace = 185,
		KeypadTab = 186,
		KeypadBackspace = 187,
		KeypadA = 188,
		KeypadB = 189,
		KeypadC = 190,
		KeypadD = 191,
		KeypadE = 192,
		KeypadF = 193,
		KeypadXor = 194,
		KeypadPower = 195,
		KeypadPercent = 196,
		KeypadLess = 197,
		KeypadGreater = 198,
		KeypadAmpersand = 199,
		KeypadColon = 203,
		KeypadHash = 204,
		KeypadSpace = 205,
		KeypadClear = 216,
		LeftControl = 224,
		LeftShift = 225,
		LeftAlt = 226,
		LeftOS = 227,
		RightControl = 228,
		RightShift = 229,
		RightAlt = 230,
		RightOS = 231,

		Count = 232
	};

	enum class MouseButton : u8
	{
		None = 0,
		Left,
		Middle,
		Right,
		X1,
		X2,
		Count,
	};

	// Physical button positions, independent of the labels printed on a gamepad.
	enum class GamepadButton : u8
	{
		South,
		East,
		West,
		North,
		Back,
		Guide,
		Start,
		LeftStick,
		RightStick,
		LeftShoulder,
		RightShoulder,
		Up,
		Down,
		Left,
		Right,

		Count
	};

	enum class GamepadAxis : u8
	{
		LeftX,
		LeftY,
		RightX,
		RightY,
		LeftTrigger,
		RightTrigger,

		Count
	};

	inline constexpr u32 MAX_GAMEPADS = 4;
	inline constexpr u32 KEY_COUNT = (u32)Key::Count;
	inline constexpr u32 KEY_WORDS = (KEY_COUNT + 63) / 64;
	inline constexpr u32 TEXT_CAPACITY = 64;

	// A stick deflection below this is noise, and does not count as the player using the pad.
	inline constexpr f32 GAMEPAD_NOISE = 0.1f;

	// A slot is stable while connected and may be reused after disconnection.
	struct Gamepad
	{
		u16 down_bits = 0;
		u16 pressed_bits = 0;
		u16 released_bits = 0;
		bool connected = false;
		f32 axes[(u32)GamepadAxis::Count] = {};
		u64 last_input_ns = 0;

		bool down(GamepadButton button) const
		{
			return (down_bits >> (u32)button & 1) != 0;
		}

		bool pressed(GamepadButton button) const
		{
			return (pressed_bits >> (u32)button & 1) != 0;
		}

		bool released(GamepadButton button) const
		{
			return (released_bits >> (u32)button & 1) != 0;
		}

		f32 axis(GamepadAxis which) const
		{
			return axes[(u32)which];
		}
	};

	struct InputState
	{
		// When the pump published this snapshot, on the engine's clock.
		u64 time_ns = 0;

		u64 keys_down[KEY_WORDS] = {};
		u64 keys_pressed[KEY_WORDS] = {};
		u64 keys_released[KEY_WORDS] = {};

		u32 mouse_down = 0;
		u32 mouse_pressed = 0;
		u32 mouse_released = 0;

		// The pointer in pixels of mouse_window, its motion this frame in points, and the wheel's
		// travel this frame in notches.
		f32 mouse_x = 0.0f;
		f32 mouse_y = 0.0f;
		f32 mouse_dx = 0.0f;
		f32 mouse_dy = 0.0f;
		f32 wheel_x = 0.0f;
		f32 wheel_y = 0.0f;

		// The window the mouse was last seen in, the one holding keyboard focus, and the one
		// whose close button was pressed this frame. Null when none of ours.
		WindowHandle mouse_window;
		WindowHandle focus;
		WindowHandle close_requested;

		// Last activity times, used to choose keyboard or gamepad prompts.
		u64 keyboard_ns = 0;
		u64 mouse_ns = 0;

		Gamepad gamepads[MAX_GAMEPADS];

		// The UTF-8 typed this frame, not terminated. Only while text input is started.
		u8 text[TEXT_CAPACITY] = {};
		u32 text_size = 0;

		bool down(Key key) const
		{
			return bit(keys_down, (u32)key);
		}

		bool pressed(Key key) const
		{
			return bit(keys_pressed, (u32)key);
		}

		bool released(Key key) const
		{
			return bit(keys_released, (u32)key);
		}

		bool down(MouseButton button) const
		{
			return (mouse_down >> (u32)button & 1) != 0;
		}

		bool pressed(MouseButton button) const
		{
			return (mouse_pressed >> (u32)button & 1) != 0;
		}

		bool released(MouseButton button) const
		{
			return (mouse_released >> (u32)button & 1) != 0;
		}

		bool control() const
		{
			return down(Key::LeftControl) || down(Key::RightControl);
		}

		bool shift() const
		{
			return down(Key::LeftShift) || down(Key::RightShift);
		}

		bool alt() const
		{
			return down(Key::LeftAlt) || down(Key::RightAlt);
		}

		// Chooses gamepad prompts only after more recent gamepad activity.
		bool gamepad_in_use() const
		{
			const u64 desk = max(keyboard_ns, mouse_ns);

			for (const Gamepad& gamepad : gamepads)
			{
				if (gamepad.connected && gamepad.last_input_ns > desk)
				{
					return true;
				}
			}

			return false;
		}

	private:
		static bool bit(const u64* words, u32 index)
		{
			return (words[index / 64] >> (index % 64) & 1) != 0;
		}
	};
}
