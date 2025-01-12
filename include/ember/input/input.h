#pragma once

#include "mouse.h"
#include "controller.h"
#include "keyboard.h"

namespace Ember
{
	static constexpr u32 MAX_CONTROLLERS = 4;

	struct InputState
	{
		Controller	controllers[MAX_CONTROLLERS];
		Keyboard	keyboard;
		Mouse		mouse;

		Controller* controller(u32 id);
	};

	class Window;
	class Input
	{
	public:
		static constexpr u32 MAX_CONTROLLERS = 4;

		static InputState&	state() { return m_state; }
		static InputState&	previous_state() { return m_previous_state; }

		static void	step_state();

		static Mouse&		mouse() { return m_state.mouse; }
		static Keyboard&	keyboard() { return m_state.keyboard; }
		static Controller&	controller(const u32 index) { return m_state.controllers[index]; }

		// callback that is called each time a text character is entered
		// static std::function<void(char value)> text_input_callback;

	private:
		static InputState m_state;
		static InputState m_previous_state;
	};
}
