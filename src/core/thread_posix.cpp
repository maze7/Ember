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
		// Truncate first: pthread_setname_np rejects names beyond the platform limit.
		constexpr size_t NAME_LIMIT = 63;
	#else
		constexpr size_t NAME_LIMIT = 15;
	#endif

	#if defined(__linux__)
		// Use the process leader's allowed CPUs, including taskset and container restrictions.
		// Reading a pinned worker's mask would incorrectly report only its assigned processor.
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
		(void)pthread_setname_np(cut);
	#else
		(void)pthread_setname_np(pthread_self(), cut);
	#endif

		EMBER_PROFILE_THREAD(name);
	}

	bool set_affinity(u32 processor)
	{
	#if defined(__linux__)
		// Processor indices are relative to the allowed set, which may contain gaps.
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
		// macOS affinity policies do not provide CPU pinning.
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
