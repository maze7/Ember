#include "core/time.h"
#include "SDL3/SDL_timer.h"

using namespace Ember;

u64 Time::m_ticks = 0;
u64 Time::m_previous_ticks = 0;
float Time::m_delta = 0;

void Time::tick() {
	u64 counter = SDL_GetPerformanceCounter();
	u64 per_second = SDL_GetPerformanceFrequency();
	m_ticks = (counter * (TICKS_PER_SECOND / per_second));
	m_delta = (m_ticks - m_previous_ticks) / TICKS_PER_SECOND;
}


