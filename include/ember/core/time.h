#pragma once

// The engine's clock. now_ns() reads the platform's steady clock in nanoseconds: it never goes
// backwards and its origin is nobody's, so one reading means nothing alone and two readings
// subtract. The logger stamps records with it, the scheduler times stalls by it and the frame
// loop will measure its stages with it. A file's modification time is on the other clock, the
// wall's, counted from the Unix epoch, and the two never meet.

#include <ember/core/types.h>

namespace ember
{
	// Nanoseconds on the steady clock. Any thread, any time, before memory::initialize() included.
	u64 now_ns();
}
