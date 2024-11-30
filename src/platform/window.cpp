#include "platform/window.h"
#include "SDL3/SDL.h"

using namespace Ember;

namespace
{
	struct Joystick { u32 instance_id; SDL_Joystick* ptr; };
	struct Gamepad  { u32 instance_id; SDL_Gamepad*  ptr; };

	std::vector<Joystick> joysticks;
	std::vector<Gamepad> gamepads;
}

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

			state.mouse.on_move({ (x - win_x), (y - win_y) }, { x, y });
		}

		switch (event.type) {
			case SDL_EVENT_QUIT:
				return false;

			/// Keyboard events
			case SDL_EVENT_KEY_DOWN:
				[[fallthrough]];
			case SDL_EVENT_KEY_UP:
				if (!event.key.repeat)
					state.keyboard.on_key(static_cast<Key>(event.key.scancode), event.type == SDL_EVENT_KEY_DOWN);
				break;

			/// Mouse events
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
				[[fallthrough]];
			case SDL_EVENT_MOUSE_BUTTON_UP: {
				MouseButton btn = MouseButton::None;
				if (event.button.button == SDL_BUTTON_LEFT)
					btn = MouseButton::Left;
				else if (event.button.button == SDL_BUTTON_RIGHT)
					btn = MouseButton::Right;
				else if (event.button.button == SDL_BUTTON_MIDDLE)
					btn = MouseButton::Middle;

				state.mouse.on_button(btn, event.type);
			}
			case SDL_EVENT_MOUSE_WHEEL:
				state.mouse.on_wheel({ event.wheel.x, event.wheel.y });
				break;

			/// Joystick events
			case SDL_EVENT_JOYSTICK_ADDED: {
				u32 instance_id = event.jdevice.which;
				if (SDL_IsGamepad(instance_id))
					break;

				// open the joystick and cache it in the joysticks vector
				auto ptr = SDL_OpenJoystick(instance_id);
				joysticks.emplace_back(instance_id, ptr);

				// search for a free controller slot and connect the joystick to that slot
				for (auto & controller : state.controllers) {
					if (controller.m_connected) {
						continue;
					} else {
						// connect the controller to the free slot
						controller.connect(
							instance_id,
							SDL_GetJoystickName(ptr),
							false,
							SDL_GetNumJoystickButtons(ptr),
							SDL_GetNumJoystickAxes(ptr),
							SDL_GetJoystickVendor(ptr),
							SDL_GetJoystickProduct(ptr),
							SDL_GetJoystickProductVersion(ptr)
						);
						break; // we've found a slot, no need to continue looping
					}
				}
				break;
			}
			case SDL_EVENT_JOYSTICK_REMOVED: {
				auto id = event.jdevice.which;
				if (SDL_IsGamepad(id))
					break;

				// disconnect Controller
				if (auto controller = state.controller(id))
					controller->disconnect();

				std::erase_if(joysticks, [id](const Joystick& joystick) {
					if (joystick.instance_id == id) {
						SDL_CloseJoystick(joystick.ptr);
						return true;
					}
					return false;
				});

				break;
			}
			case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
				[[fallthrough]];
			case SDL_EVENT_JOYSTICK_BUTTON_UP: {
				u32 id = event.jbutton.which;
				if (SDL_IsGamepad(id))
					break;

				// find Controller and forward event
				if (auto controller = state.controller(id))
					controller->on_button((Button)event.jbutton.button, event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);

				break;
			}
			case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
				u32 id = event.jaxis.which;

				if (SDL_IsGamepad(id))
					break;

				// find Controller and forward event
				if (auto controller = state.controller(id))
					controller->on_axis((Axis)event.jaxis.axis, event.jaxis.value);

				break;
			}

			// gamepad
			case SDL_EVENT_GAMEPAD_ADDED: {
				u32 id = event.gdevice.which;

				// open the gamepad and cache it in the gamepads vector
				if (auto ptr = SDL_OpenGamepad(id)) {
					gamepads.emplace_back(id, ptr);

					// search for a free controller slot and connect the gamepad to that slot
					for (auto & controller : state.controllers) {
						if (controller.m_connected) {
							continue;
						} else {
							controller.connect(
								id,
								SDL_GetGamepadName(ptr),
								false,
								15,
								6,
								SDL_GetGamepadVendor(ptr),
								SDL_GetGamepadProduct(ptr),
								SDL_GetGamepadProductVersion(ptr)
							);
							break; // we've found a slot, no need to continue looping.
						}
					}
				}
				break;
			}
			case SDL_EVENT_GAMEPAD_REMOVED: {
				u32 id = event.gdevice.which;

				// disconnect Controller
				if (auto controller = state.controller(id))
					controller->disconnect();

				// remove SDL3 gamepad
				std::erase_if(gamepads, [id](const Gamepad& gamepad) {
					if (gamepad.instance_id == id) {
						SDL_CloseGamepad(gamepad.ptr);
						return true;
					}
					return false;
				});
				break;
			}
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				[[fallthrough]];
			case SDL_EVENT_GAMEPAD_BUTTON_UP: {
				u32 id = event.gbutton.which;

				// find Controler and forward the event
				if (auto controller = state.controller(id))
					controller->on_button((Button)event.gbutton.button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);

				break;
			}
			case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
				u32 id = event.gaxis.which;

				// find Controller and forward event
				if (auto controller = state.controller(id))
					controller->on_axis((Axis)event.gaxis.axis, event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION);

				break;
			}
			default:
				break;
		}
	}

	return true;
}