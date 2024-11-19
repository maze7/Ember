#include "input/mouse.h"

using namespace Ember;

void Mouse::on_move(Vector2f position, Vector2f screen_position) {
	m_position = position;
	m_screen_position = screen_position;
}

void Mouse::on_wheel(Vector2f wheel) {
	m_wheel = wheel;
}

void Mouse::on_button(MouseButton button, bool down) {
	const u32 index = (u32) button;
	EMBER_ASSERT(index < MAX_MOUSE_BUTTONS);

	if (down) {
		m_down[index] = true;
		m_pressed[index] = true;
	} else {
		m_down[index] = false;
		m_released[index] = true;
	}
}