#pragma once

#include <functional>

#include "mouse.h"
#include "gamepad.h"
#include "keyboard.h"

namespace Ember
{
	static constexpr u32 MAX_GAMEPADS = 4;

	struct InputState
	{
		Gamepad		gamepads[MAX_GAMEPADS];
		Keyboard	keyboard;
		Mouse		mouse;
	};

	class Window;
	class Input
	{
	public:
		static InputState&	state() { return m_state; }
		static InputState&	previous_state() { return m_previous_state; }

		static Mouse&		mouse() { return m_state.mouse; }
		static Keyboard&	keyboard() { return m_state.keyboard; }
		static Gamepad&		gamepad(const u32 index) { return m_state.gamepads[index]; }

		// callback that is called each time a text character is entered
		// static std::function<void(char value)> text_input_callback;

	private:
		static InputState m_state;
		static InputState m_previous_state;
		static InputState m_next_state;
	};
}