#include <ember/core/time.h>

#include <chrono>

namespace ember
{
	u64 now_ns()
	{
		using namespace std::chrono;
		return (u64)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
	}
}
