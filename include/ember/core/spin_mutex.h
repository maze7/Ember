#pragma once

// A lock for critical sections of a few dozen instructions: taking a block from the tagged heap,
// pushing to a queue. It never sleeps the thread, so a holder that is descheduled stalls every
// waiter, and anything longer than a handful of loads and stores belongs under a lock that can
// wait.
//
// On a fiber the rule is stricter still. A job must never hold a SpinMutex across a wait: the job
// resumes on whichever worker takes it, and in between the lock would be pinning a thread that
// no longer has any reason to be pinned.
//
// The header includes <atomic> and nothing else. The pause instruction is a compiler builtin
// and the yield is one C function, because the obvious spellings, <immintrin.h> for _mm_pause
// and <thread> for std::this_thread::yield, cost a hundred thousand preprocessed lines, and this
// header sits under the memory module and so under everything.

#include <ember/core/macros.h>
#include <ember/core/types.h>

#include <atomic>

#if EMBER_COMPILER_MSVC
extern "C" __declspec(dllimport) int __stdcall SwitchToThread(void);
#else
	#include <sched.h>
#endif

namespace ember
{
	namespace internal
	{
		// Tells the core the thread is spinning. On x86 PAUSE slows the loop so it stops saturating
		// the bus; after enough turns the thread yields instead, which lets a holder that was
		// descheduled run and release.
		EMBER_FINLINE void cpu_relax(u32 spins)
		{
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
	#if EMBER_COMPILER_MSVC
			_mm_pause();
	#else
			__builtin_ia32_pause();
	#endif
#elif defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield");
#endif

			if (spins > 64)
			{
#if EMBER_COMPILER_MSVC
				SwitchToThread();
#else
				sched_yield();
#endif
			}
		}
	}

	// The lock is one flag on its own cache line, so waiters spinning on it never slow the data
	// it protects. Waiters read the flag until it clears and only then try to set it, which keeps
	// the line shared instead of bouncing it between cores on every turn.
	class alignas(EMBER_CACHE_LINE) SpinMutex final
	{
	public:
		SpinMutex() = default;

		SpinMutex(const SpinMutex&) = delete;
		SpinMutex& operator=(const SpinMutex&) = delete;

		bool try_lock()
		{
			return !m_locked.test_and_set(std::memory_order_acquire);
		}

		void lock()
		{
			u32 spins = 0;

			while (m_locked.test_and_set(std::memory_order_acquire))
			{
				while (m_locked.test(std::memory_order_relaxed))
				{
					internal::cpu_relax(spins++);
				}
			}
		}

		void unlock()
		{
			m_locked.clear(std::memory_order_release);
		}

	private:
		std::atomic_flag m_locked;
	};
}
