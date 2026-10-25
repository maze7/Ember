#pragma once

#include <ember/core/types.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

// Tracy instrumentation, enabled by CMake's EMBER_USE_TRACY and EMBER_TRACE_FIBERS options.
// Use one scope/function zone per C++ scope; nest blocks for additional zones. Scope and frame
// names must be string literals. Disabled builds leave the instrumentation in place as no-ops.
namespace ember
{
	// A zone held in a variable instead of a scope, for spans that begin and end in different
	// functions. Begin and end must run on the same thread.
	using ProfileZone = TracyCZoneCtx;

	// 0xRRGGBB zone colors, one per subsystem, for readable captures.
	inline constexpr u32 PROFILE_COLOR_INPUT = 0x00acc1;
	inline constexpr u32 PROFILE_COLOR_GAMEPLAY = 0x43a047;
	inline constexpr u32 PROFILE_COLOR_PHYSICS = 0xf4511e;
	inline constexpr u32 PROFILE_COLOR_RENDER = 0x8e24aa;
	inline constexpr u32 PROFILE_COLOR_AUDIO = 0xfdd835;
	inline constexpr u32 PROFILE_COLOR_MEMORY = 0x6d4c41;
	inline constexpr u32 PROFILE_COLOR_IO = 0x3949ab;
	inline constexpr u32 PROFILE_COLOR_WAIT = 0xb71c1c; // stalls, lock waits, sleeps
	inline constexpr u32 PROFILE_COLOR_IDLE = 0x424242; // workers spinning for work
}

// Frame boundary. Call once per frame, after swapchain present.
#define EMBER_PROFILE_FRAME() FrameMark

// CPU zones (RAII, closes at the end of enclosing scope). ZONE_NAME attaches runtime text to the
// innermost open zone.
#define EMBER_PROFILE_FUNCTION() ZoneScoped
#define EMBER_PROFILE_FUNCTION_C(color) ZoneScopedC(color)
#define EMBER_PROFILE_SCOPE(name) ZoneScopedN(name)
#define EMBER_PROFILE_SCOPE_C(name, color) ZoneScopedNC(name, color)
#define EMBER_PROFILE_ZONE_NAME(txt, size) ZoneName(txt, size)

// Memory events, attributed to a named pool so Tracy's memory view shows each category or
// allocator on its own. The name must be a string literal, or otherwise live for the whole run.
#define EMBER_PROFILE_ALLOC(ptr, size, name) TracyAllocN(ptr, size, name)
#define EMBER_PROFILE_FREE(ptr, name) TracyFreeN(ptr, name)

// Called by thread::set_name(). This Tracy function needs an explicit disabled-build guard.
#if defined(TRACY_ENABLE)
	#define EMBER_PROFILE_THREAD(name) ::tracy::SetThreadName(name)
#else
	#define EMBER_PROFILE_THREAD(name) ((void)0)
#endif
