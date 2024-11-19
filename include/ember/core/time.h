#pragma once

#include "common.h"

namespace Ember
{
	class Time
	{
	public:
		static constexpr u64 TICKS_PER_SECOND = 1000 * 100;

		static void tick();

		static u64 ticks() { return m_ticks; }

		static float delta() { return m_delta; }

	private:
		// delta time from last frame
		static float m_delta;

		// uptime, in ticks, to the start of the current frame
		static u64 m_ticks;

		// uptime, in ticks, to the start of the previous frame
		static u64 m_previous_ticks;
	};
}