#pragma once

#include "mouse_buttons.h"
#include "glm.hpp"

namespace Ember
{
	class Input;
	class Window;
	class Mouse
	{
	public:
		static constexpr u32 MAX_MOUSE_BUTTONS = 8;

		[[nodiscard]] glm::vec2 position() const { return m_position; }
		[[nodiscard]] glm::vec2 screen_position() const { return m_screen_position; }
		[[nodiscard]] glm::vec2 wheel() const { return m_wheel; }

		[[nodiscard]] bool down(MouseButton button) const;
		[[nodiscard]] bool pressed(MouseButton button) const;
		[[nodiscard]] bool released(MouseButton button) const;

	private:
		friend class Window;
		friend class Input;

		void on_button(MouseButton button, bool down);
		void on_move(const glm::vec2& position, const glm::vec2& screen_position);
		void on_wheel(const glm::vec2& wheel);
		void reset();

		// whether a button was pressed this frame
		bool m_pressed[MAX_MOUSE_BUTTONS];

		// whether a button was held this frame
		bool m_down[MAX_MOUSE_BUTTONS];

		// whether a button was released this frame
		bool m_released[MAX_MOUSE_BUTTONS];

		// mouse position (screen coordinates)
		glm::vec2 m_screen_position;

		// mouse position (window coordinates);
		glm::vec2 m_position;

		// mouse wheel value for current frame
		glm::vec2 m_wheel;
	};
}
