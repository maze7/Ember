#pragma once

#include <ember/core/types.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

// Wraps the Tracy client so engine and game code never include tracy headers directly. Tracy's own
// headers compile every macro to nothing unless TRACY_ENABLE is defined, so instrumentation can
// (and should) be left in shipping code paths.
//
// Activation is automatic; Ember's CMake sets TRACY_ENABLE from EMBER_USE_TRACY and TRACY_FIBERS
// from EMBER_TRACE_FIBERS
//
// Rules of use:
// 	- One zone macro (SCOPE / FUNCTION) per C++ scope. Open a nested block for finer grained zones.
//  - Name strings passed to SCOPE and FRAME macros must be string literals.
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

// Names the calling thread in captures, where the profiler keeps the whole name rather than the
// fifteen characters Linux keeps. Tracy has no empty stand-in for this one, so the macro is
// guarded here. thread::set_name() calls it; nothing else needs to.
#if defined(TRACY_ENABLE)
	#define EMBER_PROFILE_THREAD(name) ::tracy::SetThreadName(name)
#else
	#define EMBER_PROFILE_THREAD(name) ((void)0)
#endif
