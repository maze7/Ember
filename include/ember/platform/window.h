#pragma once

#include "core/common.h"
#include "input/input.h"

// forward declaration of SDL types
struct SDL_Window;
namespace Ember
{
	class Window
	{
	public:
		Window(const char* title, i32 width, i32 height);
		~Window();

		[[nodiscard]] i32 width() const;
		[[nodiscard]] i32 height() const;
		[[nodiscard]] Vector2i size() const;
		[[nodiscard]] float aspect_ratio() const;
		[[nodiscard]] const char* title() const;
		[[nodiscard]] SDL_Window* native_handle() const { return m_window; }

		void set_size(i32 width, i32 height);
		void set_title(const char* title);
		bool poll_events(InputState& state);

	private:
		SDL_Window* m_window;
	};
}