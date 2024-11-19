#include "input/keyboard.h"

#include "core/time.h"

using namespace Ember;

bool Keyboard::down(Key key) const {
	return m_down[(int)key];
}

bool Keyboard::pressed(Key key) const {
	return m_pressed[(int)key];
}

bool Keyboard::released(Key key) const {
	return m_released[(int)key];
}

bool Keyboard::ctrl() const {
	return down(Key::LeftControl) || down(Key::RightControl);
}

bool Keyboard::shift() const {
	return down(Key::LeftShift) || down(Key::RightShift);
}

bool Keyboard::alt() const {
	return down(Key::LeftAlt) || down(Key::RightAlt);
}

void Keyboard::on_key(Key key, bool down) {
	const u32 index = (u32) key;
	EMBER_ASSERT(index < (u32) Key::Count);

	if (down) {
		m_down[index] = true;
		m_pressed[index] = true;
		m_timestamp[index] = Time::ticks();
	} else {
		m_down[index] = false;
		m_released[index] = true;
	}
}

void Keyboard::reset() {
	std::ranges::fill(m_down, false);
	std::ranges::fill(m_pressed, false);
	std::ranges::fill(m_released, false);
	std::ranges::fill(m_timestamp, 0);
}