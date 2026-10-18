#if !defined(_WIN32)

	#include <ember/core/macros.h>
	#include <ember/core/profile.h>
	#include <ember/core/thread.h>

	#include <pthread.h>
	#include <unistd.h>

	#if defined(__linux__)
		#include <sched.h>
	#endif

namespace ember::thread
{
	namespace
	{
	#if defined(__APPLE__)
		// macOS refuses a name past sixty-three characters, Linux past fifteen, rather than
		// cutting it, so set_name() cuts it first.
		constexpr size_t NAME_LIMIT = 63;
	#else
		constexpr size_t NAME_LIMIT = 15;
	#endif

	#if defined(__linux__)
		// The processors the process may run on, which taskset, a container's cpuset or a
		// launcher may have narrowed from the machine's. Read once, from the main thread's
		// mask: a thread inherits the mask of the thread that started it, so a pinned worker's
		// own mask, or one inherited from it, would say one processor.
		const cpu_set_t& allowed()
		{
			static const cpu_set_t s_allowed = []
			{
				cpu_set_t set;
				CPU_ZERO(&set);

				if (sched_getaffinity(getpid(), sizeof(set), &set) != 0 || CPU_COUNT(&set) == 0)
				{
					CPU_ZERO(&set);

					const long online = sysconf(_SC_NPROCESSORS_ONLN);
					for (long cpu = 0; cpu < online && cpu < CPU_SETSIZE; ++cpu)
					{
						CPU_SET((int)cpu, &set);
					}
				}

				return set;
			}();

			return s_allowed;
		}
	#endif
	}

	void set_name(const char* name)
	{
		EMBER_ASSERT(name != nullptr);

		char cut[NAME_LIMIT + 1];
		size_t length = 0;
		while (name[length] != '\0' && length < NAME_LIMIT)
		{
			cut[length] = name[length];
			++length;
		}

		cut[length] = '\0';

	#if defined(__APPLE__)
		// macOS names only the calling thread, so there is no thread to pass.
		(void)pthread_setname_np(cut);
	#else
		(void)pthread_setname_np(pthread_self(), cut);
	#endif

		EMBER_PROFILE_THREAD(name);
	}

	bool set_affinity(u32 processor)
	{
	#if defined(__linux__)
		// The [processor]th processor the process may run on, which on a machine nobody
		// narrowed is processor [processor] itself.
		const cpu_set_t& set = allowed();
		u32 seen = 0;

		for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
		{
			if (!CPU_ISSET(cpu, &set))
			{
				continue;
			}

			if (seen == processor)
			{
				cpu_set_t one;
				CPU_ZERO(&one);
				CPU_SET(cpu, &one);
				return pthread_setaffinity_np(pthread_self(), sizeof(one), &one) == 0;
			}

			++seen;
		}

		return false;
	#else
		// macOS has no way to pin a thread: its affinity policy is a hint about which threads
		// share a cache, and Apple silicon ignores even that.
		(void)processor;
		return false;
	#endif
	}

	u32 hardware_count()
	{
	#if defined(__linux__)
		const int count = CPU_COUNT(&allowed());
	#else
		static const long count = sysconf(_SC_NPROCESSORS_ONLN);
	#endif

		return count > 0 ? (u32)count : 1;
	}

	void sleep(u64 ns)
	{
		timespec left = {.tv_sec = (time_t)(ns / 1'000'000'000),
		    .tv_nsec = (long)(ns % 1'000'000'000)};

		// A signal cuts the sleep short and leaves the remainder in [left].
		while (nanosleep(&left, &left) == -1 && errno == EINTR)
		{
		}
	}
}

#endif
