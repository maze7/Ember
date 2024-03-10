#pragma once

#include "core/platform.h"
#include <utility>

struct SDL_Window; // forward declaration of SDL.h SDL_Window

namespace Ember {
    class Window
    {
    public:
        Window(const char* title, i32 width, i32 height);
        ~Window();

        // TODO: Replace this with a Vec2 when I build out my own 3D math classes
        [[nodiscard]] std::pair<i32, i32> size() const;
        [[nodiscard]] i32 width() const;
        [[nodiscard]] i32 height() const;
        [[nodiscard]] SDL_Window* native_handle() const { return m_window; }

        void set_size(i32 width, i32 height);
        void set_title(const char* title);
        bool poll_events();

    private:
        SDL_Window* m_window;
    };
}