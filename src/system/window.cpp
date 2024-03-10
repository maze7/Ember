#include "system/window.h"
#include "SDL2/SDL.h"

using namespace Ember;

Window::Window(const char* title, i32 width, i32 height) {
    // If SDL hasn't already been initialized, initialize it now.
    if (!SDL_WasInit(SDL_INIT_EVERYTHING)) {
        SDL_Init(SDL_INIT_EVERYTHING);
    }

    // define SDL2 window flags
    u32 window_flags = SDL_WINDOW_RESIZABLE;
#ifdef EMBER_VULKAN
    window_flags |= SDL_WINDOW_VULKAN;
#endif

    // create the underlying SDL2 window
    m_window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height, window_flags);
}

Window::~Window() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
    }

    // TODO: This assumes only one window instance will exist at a time. For now we don't need to support multiple windows
    // But it might be worth refactoring this at some point in the future.
    if (SDL_WasInit(SDL_INIT_EVERYTHING)) {
        SDL_Quit();
    }
}

i32 Window::width() const { return size().first; }
i32 Window::height() const { return size().second; }

std::pair<i32, i32> Window::size() const {
    i32 width, height;
    SDL_GetWindowSize(m_window, &width, &height);

    return { width, height };
}

void Window::set_size(i32 width, i32 height) {
    SDL_SetWindowSize(m_window, width, height);
}

void Window::set_title(const char* title) {
    SDL_SetWindowTitle(m_window, title);
}

bool Window::poll_events() {
    SDL_Event event;

    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT)
            return true;
    }

    return false;
}