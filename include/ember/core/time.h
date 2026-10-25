#pragma once

// Monotonic time for intervals and profiling. The origin is unspecified; these readings cannot
// be compared with wall-clock timestamps such as file modification times.

#include <ember/core/types.h>

namespace ember
{
	// Nanoseconds on the steady clock. Safe on any thread, including before memory initialization.
	u64 now_ns();
}
