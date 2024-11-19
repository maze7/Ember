#include "input/input.h"

using namespace Ember;

void Input::step_state() {
	// cycle states
	m_previous_state = m_state;
	m_state = m_next_state;
	m_next_state = InputState();
}