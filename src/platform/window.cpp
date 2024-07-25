#include "platform/window.h"
#include "SDL.h"

using namespace Ember;

Window::Window(const char *title, i32 width, i32 height) {
	// If SDL hasn't been initialised, initialise it now before creating a window
	if (!SDL_WasInit(SDL_INIT_EVERYTHING))
		SDL_Init(SDL_INIT_EVERYTHING);

	u32 flags = SDL_WINDOW_RESIZABLE;

	// platform specific window flags
#ifdef EMBER_VULKAN
	flags |= SDL_WINDOW_VULKAN;
#endif

	// configure window hints
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

	// create underlying SDL2 window
	m_window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, flags);
}

Window::~Window() {
	if (m_window)
		SDL_DestroyWindow(m_window);

	// TODO: This assumes only one window instance will exist at a time. Might want to track the number of open
	// windows as a static class variable and only shut down SDL when all windows have closed?
	if (SDL_WasInit(SDL_INIT_EVERYTHING))
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

void Window::set_size(i32 width, i32 height) {
	SDL_SetWindowSize(m_window, width, height);
}

void Window::set_title(const char *title) {
	SDL_SetWindowTitle(m_window, title);
}

bool Window::poll_events() {
	SDL_Event event;
	while (SDL_PollEvent(&event) != 0) {
		if (event.type == SDL_QUIT)
			return false;
	}

	return true;
}

const char *Window::title() const {
	return SDL_GetWindowTitle(m_window);
}
