#include "input/mouse.h"

using namespace Ember;

void Mouse::on_move(Vector2f position, Vector2f screen_position) {
	m_position = position;
	m_screen_position = screen_position;
}

void Mouse::on_wheel(Vector2f wheel) {
	m_wheel = wheel;
}

void Mouse::reset() {
	std::ranges::fill(m_pressed, false);
	std::ranges::fill(m_released, false);
	m_wheel = Vector2f::zero;
}

bool Mouse::down(MouseButton button) const {
	EMBER_ASSERT((u32)button < MAX_MOUSE_BUTTONS);
	return m_down[(u32)button];
}

bool Mouse::pressed(MouseButton button) const {
	EMBER_ASSERT((u32)button < MAX_MOUSE_BUTTONS);
	return m_pressed[(u32)button];
}

bool Mouse::released(MouseButton button) const {
	EMBER_ASSERT((u32)button < MAX_MOUSE_BUTTONS);
	return m_released[(u32)button];
}

void Mouse::on_button(MouseButton button, bool down) {
	const u32 index = (u32) button;
	EMBER_ASSERT(index < MAX_MOUSE_BUTTONS);

	if (down) {
		m_pressed[index] = true;
		m_down[index] = true;
	} else {
		m_released[index] = true;
		m_down[index] = false;
	}
}
