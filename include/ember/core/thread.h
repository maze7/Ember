#pragma once

// The platform's threads, as far as the engine needs them: naming one, pinning one to a
// processor, and counting the processors there are to pin to. All three act on the calling
// thread, so they belong at the top of a thread's entry function, before it runs anything else,
// and never in a job: a job's calling thread is whichever worker picked it up, and after a wait
// it may be another.
//
// What a thread is for, and what it may do, is not decided here. The job system starts its
// workers and its I/O threads privately, claims each one's arena slot through
// memory::initialize_thread() and marks the I/O threads, because those are facts about the
// scheduler rather than the platform. The dense index a per-thread array is indexed by is the
// arena slot, in arena.h.
//
// One header, two sources: thread_posix.cpp for Linux and macOS, thread_win32.cpp for Windows,
// in the shape of the virtual memory functions.

#include <ember/core/types.h>

namespace ember::thread
{
	// Names the calling thread for the debugger, the system's tools and the profiler. Linux
	// keeps fifteen characters and cuts the rest, so a name is short and its distinguishing part
	// comes early: "ember.jobs.3", not "job system worker 3". The profiler keeps the whole name.
	// [name] is ASCII, and is copied.
	void set_name(const char* name);

	// Pins the calling thread to one logical processor, numbered from zero below
	// hardware_count(). False where the platform cannot pin, which is macOS, or when the system
	// refuses; the thread then runs wherever the system puts it, as an unpinned thread does.
	bool set_affinity(u32 processor);

	// The logical processors this process may run on, so a core with two hardware threads
	// counts twice. Fixed by the first call for the rest of the run. Never zero.
	u32 hardware_count();
}
