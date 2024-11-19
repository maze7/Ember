#include "platform/window.h"
#include "SDL3/SDL.h"

using namespace Ember;

static constexpr u32 SDL_SUBSYSTEMS = SDL_INIT_GAMEPAD | SDL_INIT_VIDEO | SDL_INIT_EVENTS;

Window::Window(const char* title, i32 width, i32 height) {
	// If SDL hasn't been initialized, initialize it now before creating a window
	if (!SDL_WasInit(SDL_SUBSYSTEMS))
		SDL_Init(SDL_SUBSYSTEMS);

	// create SDL3 window
	m_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
}

Window::~Window() {
	if (m_window)
		SDL_DestroyWindow(m_window);

	// TODO: This assumes only one window instance will exist at a time. We might want to track the number of open windows
	// as a static class variable and only shut down SDL once all windows have closed?
	if (SDL_WasInit(SDL_SUBSYSTEMS))
		SDL_Quit();
}

i32 Window::width() const {
	int width, height;
	SDL_GetWindowSize(m_window, &width, &height);

	return width;
}

i32 Window::height() const {
	int width, height;
	SDL_GetWindowSize(m_window, &width, &height);

	return height;
}

float Window::aspect_ratio() const {
	return (float) width() / (float) height();
}

void Window::set_size(i32 width, i32 height) {
	SDL_SetWindowSize(m_window, width, height);
}

void Window::set_title(const char* title) {
	SDL_SetWindowTitle(m_window, title);
}

const char* Window::title() const {
	return SDL_GetWindowTitle(m_window);
}

bool Window::poll_events(InputState& state) {
	SDL_Event event;

	while (SDL_PollEvent(&event)) {
		// ImGui_ImplSDL3_ProcessEvent(&event);

		// update the mouse each frame
		{
			int win_x, win_y;
			float x, y;

			SDL_GetWindowPosition(m_window, &win_x, &win_y);
			SDL_GetGlobalMouseState(&x, &y);

			// state.mouse.on_move({ (x - win_x), (y - win_y) }, { x, y })
		}

		switch (event.type) {
			case SDL_EVENT_QUIT:
				return false;
			case SDL_EVENT_KEY_DOWN:
				if (!event.key.repeat)
					state.keyboard.on_key(static_cast<Key>(event.key.scancode), true);
				break;
			case SDL_EVENT_KEY_UP:
				if (!event.key.repeat)
					state.keyboard.on_key(static_cast<Key>(event.key.scancode), false);
				break;
			case SDL_EVENT_TEXT_INPUT:
				// state.keyboard.on_text(event.text.text);
				break;
		}
	}

	return true;
}