#pragma once

#include "mouse_buttons.h"
#include "math/vector2.h"

namespace Ember
{
	class Input;
	class Window;
	class Mouse
	{
	public:
		static constexpr u32 MAX_MOUSE_BUTTONS = 8;

		[[nodiscard]] Vector2f position() const { return m_position; }
		[[nodiscard]] Vector2f screen_position() const { return m_screen_position; }
		[[nodiscard]] Vector2f wheel() const { return m_wheel; }

		[[nodiscard]] bool down(MouseButton button) const;
		[[nodsicard]] bool pressed(MouseButton button) const;
		[[nodiscard]] bool released(MouseButton button) const;

	private:
		friend class Window;
		friend class Input;

		void on_button(MouseButton button, bool down);
		void on_move(Vector2f position, Vector2f screen_position);
		void on_wheel(Vector2f wheel);
		void reset();

		// whether a button was pressed this frame
		bool m_pressed[MAX_MOUSE_BUTTONS];

		// whether a button was held this frame
		bool m_down[MAX_MOUSE_BUTTONS];

		// whether a button was released this frame
		bool m_released[MAX_MOUSE_BUTTONS];

		// mouse position (screen coordinates)
		Vector2f m_screen_position;

		// mouse position (window coordinates);
		Vector2f m_position;

		// mouse wheel value for current frame
		Vector2f m_wheel;
	};
}