#include "ember/core/macros.h"

#include <cstdio>
#include <cstdlib>

namespace ember
{
	bool assert_fail(const char* condition, const char* message, const char* file, int line,
	    const char* function) noexcept
	{
		// One line, one write, no allocation. Everything else about reporting a failure,
		// symbolizing a stack or filing a crash report, belongs to whoever replaces this
		// function. The default's only job is to be impossible to break.
		std::fprintf(stderr, "[assert] %s:%d in %s: %s%s%s\n", file, line, function, condition,
		    message != nullptr ? " - " : "", message != nullptr ? message : "");
		std::fflush(stderr);
		return true;
	}

	void halt() noexcept
	{
		std::abort();
	}
}
