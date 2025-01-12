#pragma once

#include <vector>

#include "input.h"
#include "keys.h"
#include "buttons.h"
#include "axes.h"

namespace Ember
{
	class VirtualButton
	{
	public:
		// Abstract interface for all button bindings (trigger, key, controller button, etc.)
		class Binding
		{
		public:
			virtual ~Binding() = default;

			[[nodiscard]] virtual bool down() const = 0;
			[[nodiscard]] virtual bool pressed() const = 0;
			[[nodiscard]] virtual bool released() const = 0;
			[[nodiscard]] virtual float value() const = 0;
			[[nodiscard]] virtual float raw_value() const = 0;
		};

		// Binding for listening to a given Key
		class KeyBinding final : public Binding
		{
		public:
			explicit KeyBinding(Key key) : m_key(key) {}

			[[nodiscard]] bool down() const {
				return Input::keyboard().down(m_key);
			}

			[[nodiscard]] bool pressed() const {
				return Input::keyboard().pressed(m_key);
			}

			[[nodiscard]] bool released() const {
				return Input::keyboard().released(m_key);
			}

			[[nodiscard]] float value() const {
				return Input::keyboard().down(m_key) ? 1.0f : 0.0f;
			}

			[[nodiscard]] float raw_value() const {
				return value();
			}
		private:
			Key m_key;
		};

		// Binding for listening to a given MouseButton
		class MouseButtonBinding final : public Binding
		{
		public:
			explicit MouseButtonBinding(MouseButton button) : m_button(button) {}

			[[nodiscard]] bool down() const {
				return Input::mouse().down(m_button);
			}

			[[nodiscard]] bool pressed() const {
				return Input::mouse().pressed(m_button);
			}

			[[nodiscard]] bool released() const {
				return Input::mouse().released(m_button);
			}

			[[nodiscard]] float value() const {
				return Input::mouse().down(m_button) ? 1.0f : 0.0f;
			}

			[[nodiscard]] float raw_value() const {
				return value();
			}

		private:
			MouseButton m_button;
		};

		VirtualButton& add(Key key);

		VirtualButton& add(MouseButton button);

		template <typename T, typename T2, typename... Args>
		VirtualButton& add(T first, T2 second, const Args&... args) {
			add(first);
			add(second, args...);
			return *this;
		}

		[[nodiscard]] bool down() const;
		[[nodiscard]] bool pressed() const;
		[[nodiscard]] bool released() const;
		[[nodiscard]] float value() const;
		[[nodiscard]] float raw_value() const;

	private:
		friend class Input;

		void update();

		std::vector<std::unique_ptr<Binding>> m_bindings;
		bool m_down = false;
		bool m_pressed = false;
		bool m_released = false;
		float m_value = 0;
		float m_raw_value = 0;
	};
}
