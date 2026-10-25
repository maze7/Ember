#pragma once

// Busy-waiting locks for short critical sections. A descheduled holder delays every waiter.
// Never hold one across a fiber wait; use JobMutex when contention should park the fiber.
// Compiler intrinsics avoid pulling platform SIMD headers into allocator users.

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
		// Pause, then yield the OS thread so a descheduled holder has a chance to run.
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

	// Isolate the flag from protected data. Poll with reads before retrying the atomic write
	// to reduce cache-line transfers under contention.
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
