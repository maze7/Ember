#pragma once

// Game actions read from the frame's input snapshot. The game defines its actions with an enum and
// writes one ActionDef row per action (its kind, how it behaves, and the bindings that drive it).

#include <ember/containers/span.h>
#include <ember/core/macros.h>
#include <ember/core/types.h>
#include <ember/input/input.h>
#include <ember/math/geometry.h>

namespace ember::input
{
	// Actions::defined keeps one bit per action.
	inline constexpr u32 MAX_ACTIONS = 64;
	inline constexpr u32 MAX_BINDINGS = 4;

	// A map reading ANY_GAMEPAD follows the connected pad used most recently.
	inline constexpr u32 ANY_GAMEPAD = ~0u;
	inline constexpr u32 NO_GAMEPAD = ~0u;

	enum class ActionKind : u8
	{
		Button,
		Axis,
		Stick
	};

	// What an axis reads while both of its directions are held.
	enum class Overlap : u8
	{
		Neutral,
		LastPressed,
		FirstPressed,
	};

	enum class SourceKind : u8
	{
		None,
		Key,
		MouseButton,
		MouseWheel,
		MouseMotion,
		GamepadButton,
		GamepadAxis,
		GamepadStick
	};

	// The component a wheel or motion source reads. Both is two-dimensional and binds only to a
	// stick.
	enum class MouseAxis : u8
	{
		X,
		Y,
		Both,
	};

	enum class GamepadStick : u8
	{
		Left,
		Right,
	};

	// One physical control, read as a scalar in [0, 1] in the direction of sign, or as a vector for
	// the two-dimensional kinds. code holds the Key, MouseButton, MouseAxis, GamepadButton,
	// GamepadAxis or GamepadStick of the kind.
	struct Source
	{
		SourceKind kind = SourceKind::None;
		u8 code = 0;
		i8 sign = 1;
	};

	constexpr Source key(Key key)
	{
		return {SourceKind::Key, (u8)key, 1};
	}

	constexpr Source button(MouseButton button)
	{
		return {SourceKind::MouseButton, (u8)button, 1};
	}

	constexpr Source button(GamepadButton button)
	{
		return {SourceKind::GamepadButton, (u8)button, 1};
	}

	constexpr Source wheel(MouseAxis axis, i8 sign = 1)
	{
		return {SourceKind::MouseWheel, (u8)axis, sign};
	}

	constexpr Source motion(MouseAxis axis, i8 sign = 1)
	{
		return {SourceKind::MouseMotion, (u8)axis, sign};
	}

	constexpr Source axis(GamepadAxis axis, i8 sign = 1)
	{
		return {SourceKind::GamepadAxis, (u8)axis, sign};
	}

	constexpr Source stick(GamepadStick stick)
	{
		return {SourceKind::GamepadStick, (u8)stick, 1};
	}

	// The sources behind one binding of an action. A button takes one source. An axis takes a
	// negative and positive pair, or one analog source reads in both directions. A stick takes four
	// cardinals as left, right, up and down, or one two-dimensional source.
	struct Binding
	{
		Source sources[4];

		constexpr Binding() = default;

		constexpr Binding(Source source) : sources{source}
		{
		}

		constexpr Binding(Source negative, Source positive) : sources{negative, positive}
		{
		}

		constexpr Binding(Source left, Source right, Source up, Source down)
		    : sources{left, right, up, down}
		{
		}
	};

	inline constexpr Binding WASD{key(Key::A), key(Key::D), key(Key::W), key(Key::S)};
	inline constexpr Binding ARROWS{key(Key::Left), key(Key::Right), key(Key::Up), key(Key::Down)};
	inline constexpr Binding DPAD{button(GamepadButton::Left), button(GamepadButton::Right),
	    button(GamepadButton::Up), button(GamepadButton::Down)};
	inline constexpr Binding LEFT_STICK = stick(GamepadStick::Left);
	inline constexpr Binding RIGHT_STICK = stick(GamepadStick::Right);
	inline constexpr Binding MOUSE_MOTION = motion(MouseAxis::Both);

	// One row of a map's table. action is the game's enum value, below MAX_ACTIONS and unique
	// within the map. Rows may come in any order, and a map need not define every action.
	struct ActionDef
	{
		u32 action = 0;
		ActionKind kind = ActionKind::Button;
		Overlap overlap = Overlap::LastPressed;

		// Pulses pressed() while held, at the map's repeat rate. Buttons and axes.
		bool repeat = false;

		// A press keeps reporting pressed() for this long unless consumed.
		u32 buffer_ns = 0;

		Binding bindings[MAX_BINDINGS];
	};
}
