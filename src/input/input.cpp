#include "input/input.h"

using namespace Ember;

// static members
InputState Input::m_state;
InputState Input::m_previous_state;

Controller* InputState::controller(u32 id) {
	for (int i = 0; i < MAX_CONTROLLERS; i++) {
		if (controllers[i].id() == id)
			return &controllers[i];
	}

	return nullptr;
}

void Input::step_state() {
	// cycle states
	m_previous_state = m_state;

	// reset current state
	m_state.keyboard.reset();
	m_state.mouse.reset();
	for (auto& controller : m_state.controllers)
		controller.reset();
}
