#pragma once

// Native thread controls. Set names and affinity at thread entry, outside jobs: a fiber may
// resume on a different worker after waiting. Scheduler ownership and arena registration live
// in the job and memory systems.

#include <ember/core/types.h>

namespace ember::thread
{
	// Copies an ASCII name for the calling thread. Linux keeps the first 15 characters;
	// the profiler keeps the full name.
	void set_name(const char* name);

	// Pins the calling thread to a logical processor below hardware_count(). Returns false
	// when unsupported or refused by the OS. macOS does not support this operation.
	bool set_affinity(u32 processor);

	// Available logical processor count, cached on first use and always at least one.
	u32 hardware_count();

	// Sleeps the calling thread for at least ns. Never call from a job: it blocks the worker.
	void sleep(u64 ns);
}
