#include "ember/core/macros.h"

#include <cstdio>
#include <cstdlib>

namespace ember
{
	bool assert_fail(const char* condition, const char* message, const char* file, int line,
	    const char* function) noexcept
	{
		// Avoid engine services here: the allocator or logger may be the source of the failure.
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
