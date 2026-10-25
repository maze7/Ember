#pragma once

// Compiler attributes and low-level checks shared across Ember. Keep dependencies minimal since
// this header is included widely.

// Test Clang first because clang-cl also defines _MSC_VER.
#if defined(__clang__)
	#define EMBER_COMPILER_CLANG 1
	#define EMBER_COMPILER_GCC 0
	#define EMBER_COMPILER_MSVC 0
#elif defined(_MSC_VER)
	#define EMBER_COMPILER_CLANG 0
	#define EMBER_COMPILER_GCC 0
	#define EMBER_COMPILER_MSVC 1
#elif defined(__GNUC__)
	#define EMBER_COMPILER_CLANG 0
	#define EMBER_COMPILER_GCC 1
	#define EMBER_COMPILER_MSVC 0
#else
	#error "Ember supports Clang, GCC and MSVC."
#endif

// Use EMBER_FINLINE when profiling shows an uninlined call matters; extra inlining can increase
// code size and instruction-cache pressure. EMBER_NOINLINE also keeps TLS lookups separate
// when a fiber must read its current worker after a wait.
#if EMBER_COMPILER_MSVC
	#define EMBER_FINLINE __forceinline
	#define EMBER_NOINLINE __declspec(noinline)
#else
	#define EMBER_FINLINE inline __attribute__((always_inline))
	#define EMBER_NOINLINE __attribute__((noinline))
#endif

// Breaks into an attached debugger on Clang and MSVC. On GCC this terminates execution, so set a
// breakpoint in assert_fail() to inspect state first.
#if EMBER_COMPILER_MSVC
	#define EMBER_DEBUG_BREAK() __debugbreak()
#elif EMBER_COMPILER_CLANG
	#define EMBER_DEBUG_BREAK() __builtin_debugtrap()
#else
	#define EMBER_DEBUG_BREAK() __builtin_trap()
#endif

// Diagnoses borrowed storage that may dangle. Does not extend its lifetime; expands to nothing
// when the compiler has no supported attribute.
#if EMBER_COMPILER_CLANG
	#define EMBER_LIFETIMEBOUND [[clang::lifetimebound]]
#elif EMBER_COMPILER_MSVC && _MSC_VER >= 1932
	#define EMBER_LIFETIMEBOUND [[msvc::lifetimebound]]
#else
	#define EMBER_LIFETIMEBOUND
#endif

// Separation for independently written data to reduce false sharing. Override for targets
// that need more than 64 bytes.
#ifndef EMBER_CACHE_LINE
	#define EMBER_CACHE_LINE 64
#endif

static_assert((EMBER_CACHE_LINE & (EMBER_CACHE_LINE - 1)) == 0,
    "EMBER_CACHE_LINE must be a power of two");

// Profiling builds retain assertions even with NDEBUG.
#if !defined(NDEBUG) || defined(EMBER_PROFILE)
	#define EMBER_ASSERTS_ENABLED 1
#else
	#define EMBER_ASSERTS_ENABLED 0
#endif

// How much the memory module records about heap traffic. Level 1 counts bytes per category,
// warns when a budget is crossed and feeds Tracy; level 2 adds poison fills and breaking on an
// allocation by number. Debug builds get 2, profiling builds 1 and shipping builds 0, and a
// build may set its own level.
#ifndef EMBER_MEMORY_TRACKING
	#if !defined(NDEBUG)
		#define EMBER_MEMORY_TRACKING 2
	#elif defined(EMBER_PROFILE)
		#define EMBER_MEMORY_TRACKING 1
	#else
		#define EMBER_MEMORY_TRACKING 0
	#endif
#endif

namespace ember
{
	// Reports to stderr without requiring the logger. Returns whether to break into the debugger.
	// Only message may be null.
	[[nodiscard]] bool assert_fail(const char* condition, const char* message, const char* file,
	    int line, const char* function) noexcept;

	// Terminates the process. Kept out of line to avoid exposing <cstdlib> to callers.
	[[noreturn]] void halt() noexcept;
}

// Checks an internal invariant. Failure indicates a programmer error; use normal error handling for
// invalid input or expected runtime failures.
//
// [expr] must have no side effects because disabled assertions do not evaluate it. With assertions
// enabled, a failed check prevents constant evaluation.
#if EMBER_ASSERTS_ENABLED
	#define EMBER_ASSERT_MSG(expr, message)                                                        \
		do                                                                                         \
		{                                                                                          \
			if (!(expr))                                                                           \
			{                                                                                      \
				if (::ember::assert_fail(#expr, message, __FILE__, __LINE__, __FUNCTION__))        \
				{                                                                                  \
					EMBER_DEBUG_BREAK();                                                           \
				}                                                                                  \
			}                                                                                      \
		} while (false)
#else
    // Type-check without evaluation and retain references to assertion-only variables.
	#define EMBER_ASSERT_MSG(expr, message) ((void)sizeof(!(expr)))
#endif

#define EMBER_ASSERT(expr) EMBER_ASSERT_MSG(expr, nullptr)

// Marks a path that must never execute.
//
// With assertions enabled, reports a failure and aborts, allowing a debugger break first. With
// assertions disabled, tells the optimizer that the path is impossible. Reaching it then is
// undefined behaviour.
#if EMBER_ASSERTS_ENABLED
	#define EMBER_UNREACHABLE()                                                                    \
		do                                                                                         \
		{                                                                                          \
			if (::ember::assert_fail("unreachable code reached", nullptr, __FILE__, __LINE__,      \
			        __FUNCTION__))                                                                 \
			{                                                                                      \
				EMBER_DEBUG_BREAK();                                                               \
			}                                                                                      \
			::ember::halt();                                                                       \
		} while (false)
#elif EMBER_COMPILER_MSVC
	#define EMBER_UNREACHABLE() __assume(false)
#else
	#define EMBER_UNREACHABLE() __builtin_unreachable()
#endif
