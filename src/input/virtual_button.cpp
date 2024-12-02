#include "input/virtual_button.h"
#include <memory>
#include <ranges>
#include <algorithm>

using namespace Ember;

VirtualButton& VirtualButton::add(Key key) {
	m_bindings.push_back(std::make_unique<KeyBinding>(key));
	return *this;
}

VirtualButton& VirtualButton::add(MouseButton button) {
	m_bindings.push_back(std::make_unique<MouseButtonBinding>(button));
	return *this;
}

bool VirtualButton::down() const {
	for (const auto& binding : m_bindings) {
		if (binding->down())
			return true;
	}

	return false;
}

bool VirtualButton::pressed() const {
	for (const auto& binding : m_bindings) {
		if (binding->pressed())
			return true;
	}

	return false;
}

bool VirtualButton::released() const {
	for (const auto& binding : m_bindings) {
		if (binding->released())
			return true;
	}

	return false;
}

float VirtualButton::value() const {
	float max_value = 0.0;

	for (const auto& binding : m_bindings) {
		max_value = std::max(max_value, binding->value());
	}

	return max_value;
}

float VirtualButton::raw_value() const {
	float max_value = 0.0;

	for (const auto& binding : m_bindings) {
		max_value = std::max(max_value, binding->raw_value());
	}

	return max_value;
}

void VirtualButton::update() {}

