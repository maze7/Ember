// SDL3 backend for windows, input and Vulkan surfaces. SDL allocations use the engine heap;
// windows and gamepads stay within the capacities established at initialization.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include <ember/platform/platform.h>

#include <ember/containers/pool.h>
#include <ember/core/jobs.h>
#include <ember/core/log.h>
#include <ember/core/time.h>
#include <ember/memory/heap.h>
#include <platform/input_state.h>
#include <platform/wsi.h>

#include <cmath>
#include <cstring>

namespace ember::platform
{
	namespace
	{
		constexpr SDL_InitFlags SUBSYSTEMS = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD;
		constexpr u32 NO_GAMEPAD = ~0u;

		struct SdlWindow
		{
			SDL_Window* handle = nullptr;
			SDL_WindowID id = 0;
			CursorMode cursor = CursorMode::Normal;
		};

		struct SdlGamepad
		{
			SDL_Gamepad* handle = nullptr;
			SDL_JoystickID id = 0;
		};

		struct Platform
		{
			bool initialized = false;
			bool gamepads_discovered = false;
			PlatformDef def;
			Pool<SdlWindow, Window> windows;
			SdlGamepad gamepads[MAX_GAMEPADS];
			SDL_Cursor* cursors[(u32)SystemCursor::Count] = {};
			InputState pending;
		};

		Platform s_platform;

		// SDL's allocator, so its windows, events and pad state live in the heap under Platform.
		void* SDLCALL sdl_malloc(size_t size)
		{
			return memory::allocate(memory::Tag::Platform, size);
		}

		void* SDLCALL sdl_calloc(size_t count, size_t size)
		{
			return memory::allocate_zeroed(memory::Tag::Platform, count * size);
		}

		void* SDLCALL sdl_realloc(void* ptr, size_t size)
		{
			return memory::reallocate(memory::Tag::Platform, ptr, size);
		}

		void SDLCALL sdl_free(void* ptr)
		{
			memory::free(ptr);
		}

		void log_sdl_failure(const char* operation)
		{
			const char* error = SDL_GetError();
			EMBER_ERROR("{} failed: {}", operation, error != nullptr ? error : "unknown SDL error");
		}

		SdlWindow* find_window(WindowHandle handle)
		{
			SdlWindow* window = s_platform.windows.get(handle);
			EMBER_ASSERT_MSG(window != nullptr, "a window that is not ours or is gone");
			return window;
		}

		// The fixed window pool is small enough to resolve SDL event IDs without another index.
		WindowHandle window_from_id(SDL_WindowID id)
		{
			for (auto it = s_platform.windows.begin(); it != s_platform.windows.end(); ++it)
			{
				if ((*it).id == id)
				{
					return it.handle();
				}
			}

			return WindowHandle{};
		}

		u32 gamepad_slot(SDL_JoystickID id)
		{
			for (u32 index = 0; index < MAX_GAMEPADS; ++index)
			{
				const SdlGamepad& gamepad = s_platform.gamepads[index];
				if (gamepad.handle != nullptr && gamepad.id == id)
				{
					return index;
				}
			}

			return NO_GAMEPAD;
		}

		void open_gamepad(SDL_JoystickID id, u64 now_ns)
		{
			if (gamepad_slot(id) != NO_GAMEPAD)
			{
				return;
			}

			u32 slot = NO_GAMEPAD;
			for (u32 index = 0; index < MAX_GAMEPADS; ++index)
			{
				if (s_platform.gamepads[index].handle == nullptr)
				{
					slot = index;
					break;
				}
			}

			if (slot == NO_GAMEPAD)
			{
				EMBER_WARN("ignoring gamepad {}: every slot is taken", id);
				return;
			}

			SDL_Gamepad* native = SDL_OpenGamepad(id);
			if (native == nullptr)
			{
				log_sdl_failure("SDL_OpenGamepad");
				return;
			}

			s_platform.gamepads[slot] = {.handle = native, .id = id};
			fold_gamepad_connection(s_platform.pending, slot, true, now_ns);
		}

		void close_gamepad(u32 slot, u64 now_ns)
		{
			SdlGamepad& gamepad = s_platform.gamepads[slot];
			(void)SDL_RumbleGamepad(gamepad.handle, 0, 0, 0);
			SDL_CloseGamepad(gamepad.handle);
			gamepad = {};
			fold_gamepad_connection(s_platform.pending, slot, false, now_ns);
		}

		// Retry discovery after a removal so a connected gamepad can take the freed slot.
		void discover_gamepads(u64 now_ns)
		{
			int count = 0;
			SDL_JoystickID* ids = SDL_GetGamepads(&count);
			if (ids == nullptr)
			{
				log_sdl_failure("SDL_GetGamepads");
				return;
			}

			for (int i = 0; i < count; ++i)
			{
				open_gamepad(ids[i], now_ns);
			}

			SDL_free(ids);
			s_platform.gamepads_discovered = true;
		}

		Key key_from_sdl(SDL_Scancode scancode)
		{
			const i32 value = (i32)scancode;
			return value > 0 && value < (i32)Key::Count ? (Key)value : Key::Unknown;
		}

		MouseButton mouse_button_from_sdl(u8 button)
		{
			switch (button)
			{
				case SDL_BUTTON_LEFT:
					return MouseButton::Left;
				case SDL_BUTTON_MIDDLE:
					return MouseButton::Middle;
				case SDL_BUTTON_RIGHT:
					return MouseButton::Right;
				case SDL_BUTTON_X1:
					return MouseButton::X1;
				case SDL_BUTTON_X2:
					return MouseButton::X2;
				default:
					return MouseButton::None;
			}
		}

		GamepadType gamepad_type_from_sdl(SDL_GamepadType type)
		{
			switch (type)
			{
				case SDL_GAMEPAD_TYPE_STANDARD:
					return GamepadType::Standard;
				case SDL_GAMEPAD_TYPE_XBOX360:
					return GamepadType::Xbox360;
				case SDL_GAMEPAD_TYPE_XBOXONE:
					return GamepadType::XboxOne;
				case SDL_GAMEPAD_TYPE_PS3:
					return GamepadType::PlayStation3;
				case SDL_GAMEPAD_TYPE_PS4:
					return GamepadType::PlayStation4;
				case SDL_GAMEPAD_TYPE_PS5:
					return GamepadType::PlayStation5;
				case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
					return GamepadType::SwitchPro;
				case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
					return GamepadType::SwitchJoyConLeft;
				case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
					return GamepadType::SwitchJoyConRight;
				case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
					return GamepadType::SwitchJoyConPair;
				case SDL_GAMEPAD_TYPE_GAMECUBE:
					return GamepadType::GameCube;
				case SDL_GAMEPAD_TYPE_STEAM:
					return GamepadType::Steam;
				default:
					return GamepadType::Unknown;
			}
		}

		// The button and axis enums are SDL's in SDL's order, so an event's number is ours.
		static_assert((int)GamepadButton::South == SDL_GAMEPAD_BUTTON_SOUTH);
		static_assert((int)GamepadButton::Right == SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
		static_assert((int)GamepadButton::Count == SDL_GAMEPAD_BUTTON_MISC1);
		static_assert((int)GamepadAxis::RightTrigger == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		static_assert((int)GamepadAxis::Count == SDL_GAMEPAD_AXIS_COUNT);

		constexpr SDL_SystemCursor SYSTEM_CURSORS[(u32)SystemCursor::Count] = {
		    SDL_SYSTEM_CURSOR_DEFAULT,
		    SDL_SYSTEM_CURSOR_TEXT,
		    SDL_SYSTEM_CURSOR_WAIT,
		    SDL_SYSTEM_CURSOR_CROSSHAIR,
		    SDL_SYSTEM_CURSOR_PROGRESS,
		    SDL_SYSTEM_CURSOR_NWSE_RESIZE,
		    SDL_SYSTEM_CURSOR_NESW_RESIZE,
		    SDL_SYSTEM_CURSOR_EW_RESIZE,
		    SDL_SYSTEM_CURSOR_NS_RESIZE,
		    SDL_SYSTEM_CURSOR_MOVE,
		    SDL_SYSTEM_CURSOR_NOT_ALLOWED,
		    SDL_SYSTEM_CURSOR_POINTER,
		    SDL_SYSTEM_CURSOR_NW_RESIZE,
		    SDL_SYSTEM_CURSOR_N_RESIZE,
		    SDL_SYSTEM_CURSOR_NE_RESIZE,
		    SDL_SYSTEM_CURSOR_E_RESIZE,
		    SDL_SYSTEM_CURSOR_SE_RESIZE,
		    SDL_SYSTEM_CURSOR_S_RESIZE,
		    SDL_SYSTEM_CURSOR_SW_RESIZE,
		    SDL_SYSTEM_CURSOR_W_RESIZE,
		};

		// SDL reports sticks as signed 16 bit, with one more step below zero than above.
		f32 axis_from_sdl(Sint16 value)
		{
			return value >= 0 ? (f32)value / 32767.0f : (f32)value / 32768.0f;
		}

		Uint16 rumble_from_intensity(f32 value)
		{
			if (!(value > 0.0f))
			{
				return 0;
			}

			return value >= 1.0f ? 0xffff : (Uint16)(value * 65535.0f + 0.5f);
		}

		SDL_WindowFlags window_flags_from(WindowFlags flags)
		{
			SDL_WindowFlags result = 0;

			if (has_any(flags, WindowFlags::Resizable))
			{
				result |= SDL_WINDOW_RESIZABLE;
			}

			if (has_any(flags, WindowFlags::Fullscreen))
			{
				result |= SDL_WINDOW_FULLSCREEN;
			}

			if (has_any(flags, WindowFlags::Hidden))
			{
				result |= SDL_WINDOW_HIDDEN;
			}

			if (has_any(flags, WindowFlags::Borderless))
			{
				result |= SDL_WINDOW_BORDERLESS;
			}

			if (has_any(flags, WindowFlags::HighPixelDensity))
			{
				result |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
			}

			if (s_platform.def.vulkan)
			{
				result |= SDL_WINDOW_VULKAN;
			}

			return result;
		}

		// Convert position to swapchain pixels. Keep motion in points so display scaling does
		// not change mouse-look sensitivity.
		f32 pixel_density(WindowHandle handle)
		{
			const SdlWindow* window = s_platform.windows.get(handle);
			const f32 density =
			    window != nullptr ? SDL_GetWindowPixelDensity(window->handle) : 0.0f;
			return density > 0.0f ? density : 1.0f;
		}

		bool from_touch_or_pen(SDL_MouseID which)
		{
			return which == SDL_TOUCH_MOUSEID || which == SDL_PEN_MOUSEID;
		}

		void fold_event(const SDL_Event& event, u64 now_ns, bool& quit)
		{
			InputState& pending = s_platform.pending;

			switch (event.type)
			{
				case SDL_EVENT_QUIT:
					quit = true;
					break;

				case SDL_EVENT_KEY_DOWN:
				case SDL_EVENT_KEY_UP:
					if (!event.key.repeat)
					{
						fold_key(pending, key_from_sdl(event.key.scancode), event.key.down, now_ns);
					}
					break;

				case SDL_EVENT_TEXT_INPUT:
					if (event.text.text != nullptr)
					{
						fold_text(pending, event.text.text, (u32)std::strlen(event.text.text));
					}
					break;

				case SDL_EVENT_MOUSE_MOTION:
				{
					const WindowHandle window = window_from_id(event.motion.windowID);
					if (!from_touch_or_pen(event.motion.which) && !window.is_null())
					{
						const f32 density = pixel_density(window);
						fold_mouse_move(pending, event.motion.x * density, event.motion.y * density,
						    event.motion.xrel, event.motion.yrel, window, now_ns);
					}
					break;
				}

				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
				{
					const WindowHandle window = window_from_id(event.button.windowID);
					if (!from_touch_or_pen(event.button.which) && !window.is_null())
					{
						fold_mouse_button(pending, mouse_button_from_sdl(event.button.button),
						    event.button.down, window, now_ns);
					}
					break;
				}

				case SDL_EVENT_MOUSE_WHEEL:
				{
					const WindowHandle window = window_from_id(event.wheel.windowID);
					if (!from_touch_or_pen(event.wheel.which) && !window.is_null())
					{
						const f32 sign =
						    event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
						fold_mouse_wheel(pending, event.wheel.x * sign, event.wheel.y * sign,
						    window, now_ns);
					}
					break;
				}

				case SDL_EVENT_WINDOW_FOCUS_GAINED:
				case SDL_EVENT_WINDOW_FOCUS_LOST:
				{
					const WindowHandle window = window_from_id(event.window.windowID);
					if (!window.is_null())
					{
						fold_focus(pending, window, event.type == SDL_EVENT_WINDOW_FOCUS_GAINED,
						    now_ns);
					}
					break;
				}

				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				{
					const WindowHandle window = window_from_id(event.window.windowID);
					if (!window.is_null())
					{
						fold_close_request(pending, window);
					}
					break;
				}

				case SDL_EVENT_GAMEPAD_ADDED:
					open_gamepad(event.gdevice.which, now_ns);
					break;

				case SDL_EVENT_GAMEPAD_REMOVED:
				{
					const u32 slot = gamepad_slot(event.gdevice.which);
					if (slot != NO_GAMEPAD)
					{
						close_gamepad(slot, now_ns);
						s_platform.gamepads_discovered = false;
					}
					break;
				}

				case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				case SDL_EVENT_GAMEPAD_BUTTON_UP:
				{
					const u32 slot = gamepad_slot(event.gbutton.which);
					if (slot != NO_GAMEPAD && event.gbutton.button < (u8)GamepadButton::Count)
					{
						fold_gamepad_button(pending, slot, (GamepadButton)event.gbutton.button,
						    event.gbutton.down, now_ns);
					}
					break;
				}

				case SDL_EVENT_GAMEPAD_AXIS_MOTION:
				{
					const u32 slot = gamepad_slot(event.gaxis.which);
					if (slot != NO_GAMEPAD && event.gaxis.axis < (u8)GamepadAxis::Count)
					{
						fold_gamepad_axis(pending, slot, (GamepadAxis)event.gaxis.axis,
						    axis_from_sdl(event.gaxis.value), now_ns);
					}
					break;
				}

				default:
					break;
			}
		}
	}

	bool initialize(const PlatformDef& def)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT_MSG(!s_platform.initialized, "initialize runs once");

		s_platform.def = def;

		// Install allocators before SDL can allocate, including through SDL_SetHint().
		if (!SDL_SetMemoryFunctions(sdl_malloc, sdl_calloc, sdl_realloc, sdl_free))
		{
			EMBER_ERROR("SDL would not take the engine's allocator");
			return false;
		}

		SDL_SetMainReady();
		(void)SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
		    def.background_gamepads ? "1" : "0");

		if (!SDL_Init(SUBSYSTEMS))
		{
			log_sdl_failure("SDL_Init");
			return false;
		}

		s_platform.windows.initialize(memory::heap(), memory::Tag::Platform,
		    def.max_windows > 0 ? def.max_windows : 1);
		s_platform.pending = InputState{};
		s_platform.gamepads_discovered = false;
		s_platform.initialized = true;
		return true;
	}

	void shutdown()
	{
		EMBER_ASSERT(jobs::is_main());

		if (!s_platform.initialized)
		{
			return;
		}

		const u64 now = now_ns();
		for (u32 slot = 0; slot < MAX_GAMEPADS; ++slot)
		{
			if (s_platform.gamepads[slot].handle != nullptr)
			{
				close_gamepad(slot, now);
			}
		}

		for (SDL_Cursor*& cursor : s_platform.cursors)
		{
			if (cursor != nullptr)
			{
				SDL_DestroyCursor(cursor);
				cursor = nullptr;
			}
		}

		for (auto it = s_platform.windows.begin(); it != s_platform.windows.end(); ++it)
		{
			SDL_DestroyWindow((*it).handle);
		}

		s_platform.windows.shutdown();
		SDL_Quit();

		s_platform.pending = InputState{};
		s_platform.initialized = false;
	}

	bool pump(InputState& input)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT_MSG(s_platform.initialized, "pump before initialize");

		const u64 now = now_ns();
		bool quit = false;

		if (!s_platform.gamepads_discovered)
		{
			discover_gamepads(now);
		}

		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			fold_event(event, now, quit);
		}

		publish(s_platform.pending, input, now);
		return quit;
	}

	WindowHandle create_window(const WindowDef& def)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT_MSG(s_platform.initialized, "create_window before initialize");

		if (def.width == 0 || def.height == 0)
		{
			EMBER_WARN("a window of {}x{} points", def.width, def.height);
			return WindowHandle{};
		}

		// No bigger than the display's usable area, in points, and centred on it.
		const SDL_DisplayID display = SDL_GetPrimaryDisplay();
		u32 width = def.width;
		u32 height = def.height;
		SDL_Rect usable;
		if (SDL_GetDisplayUsableBounds(display, &usable))
		{
			width = width < (u32)usable.w ? width : (u32)usable.w;
			height = height < (u32)usable.h ? height : (u32)usable.h;
		}

		const SDL_PropertiesID props = SDL_CreateProperties();
		SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, def.title);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
		    SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
		    SDL_WINDOWPOS_CENTERED_DISPLAY(display));
		SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
		    (Sint64)window_flags_from(def.flags));

		SDL_Window* native = SDL_CreateWindowWithProperties(props);
		SDL_DestroyProperties(props);

		if (native == nullptr)
		{
			log_sdl_failure("SDL_CreateWindowWithProperties");
			return WindowHandle{};
		}

		const WindowHandle handle =
		    s_platform.windows.emplace(SdlWindow{.handle = native, .id = SDL_GetWindowID(native)});

		if (handle.is_null())
		{
			EMBER_ERROR("no room for another window: PlatformDef.max_windows is {}",
			    s_platform.windows.capacity());
			SDL_DestroyWindow(native);
		}

		return handle;
	}

	void destroy_window(WindowHandle handle)
	{
		EMBER_ASSERT(jobs::is_main());

		SdlWindow* window = find_window(handle);
		if (window == nullptr)
		{
			return;
		}

		SDL_DestroyWindow(window->handle);
		(void)s_platform.windows.erase(handle);
	}

	Extent2D window_size_in_pixels(WindowHandle handle)
	{
		EMBER_ASSERT(jobs::is_main());

		const SdlWindow* window = find_window(handle);
		int width = 0;
		int height = 0;
		if (window == nullptr || !SDL_GetWindowSizeInPixels(window->handle, &width, &height))
		{
			return Extent2D{};
		}

		return Extent2D{.width = width > 0 ? (u32)width : 0,
		    .height = height > 0 ? (u32)height : 0};
	}

	f32 window_display_scale(WindowHandle handle)
	{
		EMBER_ASSERT(jobs::is_main());

		const SdlWindow* window = find_window(handle);
		const f32 scale = window != nullptr ? SDL_GetWindowDisplayScale(window->handle) : 0.0f;
		return scale > 0.0f ? scale : 1.0f;
	}

	void set_window_title(WindowHandle handle, const char* title)
	{
		EMBER_ASSERT(jobs::is_main());

		if (const SdlWindow* window = find_window(handle))
		{
			if (!SDL_SetWindowTitle(window->handle, title))
			{
				log_sdl_failure("SDL_SetWindowTitle");
			}
		}
	}

	void set_window_size(WindowHandle handle, u32 width, u32 height)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT(width > 0 && height > 0 && width <= 0x7fffffffu && height <= 0x7fffffffu);

		if (const SdlWindow* window = find_window(handle))
		{
			if (!SDL_SetWindowSize(window->handle, (int)width, (int)height))
			{
				log_sdl_failure("SDL_SetWindowSize");
			}
		}
	}

	void set_cursor_mode(WindowHandle handle, CursorMode mode)
	{
		EMBER_ASSERT(jobs::is_main());

		SdlWindow* window = find_window(handle);
		if (window == nullptr || window->cursor == mode)
		{
			return;
		}

		window->cursor = mode;

		if (!SDL_SetWindowRelativeMouseMode(window->handle, mode == CursorMode::Relative))
		{
			log_sdl_failure("SDL_SetWindowRelativeMouseMode");
			return;
		}

		if (mode != CursorMode::Relative)
		{
			const bool shown = mode == CursorMode::Normal ? SDL_ShowCursor() : SDL_HideCursor();
			if (!shown)
			{
				log_sdl_failure(mode == CursorMode::Normal ? "SDL_ShowCursor" : "SDL_HideCursor");
			}
		}
	}

	void set_cursor(SystemCursor cursor)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT((u32)cursor < (u32)SystemCursor::Count);

		SDL_Cursor*& native = s_platform.cursors[(u32)cursor];
		if (native == nullptr)
		{
			native = SDL_CreateSystemCursor(SYSTEM_CURSORS[(u32)cursor]);
			if (native == nullptr)
			{
				log_sdl_failure("SDL_CreateSystemCursor");
				return;
			}
		}

		if (!SDL_SetCursor(native))
		{
			log_sdl_failure("SDL_SetCursor");
		}
	}

	void start_text_input(WindowHandle handle)
	{
		EMBER_ASSERT(jobs::is_main());

		if (const SdlWindow* window = find_window(handle))
		{
			if (!SDL_StartTextInput(window->handle))
			{
				log_sdl_failure("SDL_StartTextInput");
			}
		}
	}

	void stop_text_input(WindowHandle handle)
	{
		EMBER_ASSERT(jobs::is_main());

		if (const SdlWindow* window = find_window(handle))
		{
			if (!SDL_StopTextInput(window->handle))
			{
				log_sdl_failure("SDL_StopTextInput");
			}
		}
	}

	bool clipboard_text(String& out)
	{
		EMBER_ASSERT(jobs::is_main());

		// SDL's copy comes from the heap it was given and goes back through SDL_free.
		char* text = SDL_GetClipboardText();
		if (text == nullptr)
		{
			log_sdl_failure("SDL_GetClipboardText");
			out.clear();
			return false;
		}

		out.assign(text);
		SDL_free(text);
		return true;
	}

	void set_clipboard_text(const char* text)
	{
		EMBER_ASSERT(jobs::is_main());

		if (!SDL_SetClipboardText(text))
		{
			log_sdl_failure("SDL_SetClipboardText");
		}
	}

	bool gamepad_info(u32 index, GamepadInfo& out)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT(index < MAX_GAMEPADS);

		SDL_Gamepad* gamepad = s_platform.gamepads[index].handle;
		out = GamepadInfo{};
		if (gamepad == nullptr)
		{
			return false;
		}

		if (const char* name = SDL_GetGamepadName(gamepad))
		{
			const size_t length = std::strlen(name);
			const size_t kept = length < sizeof(out.name) - 1 ? length : sizeof(out.name) - 1;
			std::memcpy(out.name, name, kept);
			out.name[kept] = '\0';
		}

		out.type = gamepad_type_from_sdl(SDL_GetGamepadType(gamepad));
		out.vendor = SDL_GetGamepadVendor(gamepad);
		out.product = SDL_GetGamepadProduct(gamepad);
		out.product_version = SDL_GetGamepadProductVersion(gamepad);
		return true;
	}

	void gamepad_rumble(u32 index, f32 low, f32 high, u32 duration_ms)
	{
		EMBER_ASSERT(jobs::is_main());
		EMBER_ASSERT(index < MAX_GAMEPADS);

		SDL_Gamepad* gamepad = s_platform.gamepads[index].handle;
		if (gamepad == nullptr)
		{
			return;
		}

		// Unsupported rumble is expected for some gamepads; avoid logging it every frame.
		(void)SDL_RumbleGamepad(gamepad, rumble_from_intensity(low), rumble_from_intensity(high),
		    duration_ms);
	}

	namespace vulkan
	{
		ProcAddress load()
		{
			EMBER_ASSERT(jobs::is_main());

			if (!SDL_Vulkan_LoadLibrary(nullptr))
			{
				log_sdl_failure("SDL_Vulkan_LoadLibrary");
				return nullptr;
			}

			const SDL_FunctionPointer proc = SDL_Vulkan_GetVkGetInstanceProcAddr();
			if (proc == nullptr)
			{
				log_sdl_failure("SDL_Vulkan_GetVkGetInstanceProcAddr");
				SDL_Vulkan_UnloadLibrary();
			}

			return proc;
		}

		void unload()
		{
			EMBER_ASSERT(jobs::is_main());
			SDL_Vulkan_UnloadLibrary();
		}

		Span<const char* const> instance_extensions()
		{
			Uint32 count = 0;
			const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
			if (names == nullptr)
			{
				log_sdl_failure("SDL_Vulkan_GetInstanceExtensions");
				return Span<const char* const>{};
			}

			return Span<const char* const>(names, count);
		}

		bool presentation_supported(VkInstance instance, VkPhysicalDevice device, u32 queue_family)
		{
			return SDL_Vulkan_GetPresentationSupport(instance, device, queue_family);
		}

		VkSurfaceKHR create_surface(WindowHandle handle, VkInstance instance)
		{
			EMBER_ASSERT(jobs::is_main());

			const SdlWindow* window = find_window(handle);
			if (window == nullptr)
			{
				return nullptr;
			}

			VkSurfaceKHR surface = nullptr;
			if (!SDL_Vulkan_CreateSurface(window->handle, instance, nullptr, &surface))
			{
				log_sdl_failure("SDL_Vulkan_CreateSurface");
				return nullptr;
			}

			return surface;
		}

		void destroy_surface(VkInstance instance, VkSurfaceKHR surface)
		{
			if (surface != nullptr)
			{
				SDL_Vulkan_DestroySurface(instance, surface, nullptr);
			}
		}
	}
}
