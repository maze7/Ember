#pragma once

// Compiler attributes and low-level checks shared across Ember. Keep dependencies minimal
// since this header is included widely.

// Exactly one compiler flag is 1. Test Clang first because clang-cl also defines _MSC_VER
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

// EMBER_FINLINE asks the compiler to inline a functon more aggressively. Use it when
// profiling shows that an uninlined call matters. Inlining large functions can increase
// code size and instruction cache pressure, so don't over-use it.
//
// EMBER_NOINLINE keeps cold paths, such as error reporting, out of callers.
#if EMBER_COMPILER_MSVC
	#define EMBER_FINLINE __forceinline
	#define EMBER_NOINLINE __declspec(noinline)
#else
	#define EMBER_FINLINE inline __attribute__((always_inline))
	#define EMBER_NOINLINE __attribute__((noinline))
#endif

// Breaks into an attached debugger on Clang and MSVC. On GCC this terminates execution,
// so set a breakpoint in assert_fail() to inspect state first.
#if EMBER_COMPILER_MSVC
	#define EMBER_DEBUG_BREAK() __debugbreak()
#elif EMBER_COMPILER_CLANG
	#define EMBER_DEBUG_BREAK() __builtin_debugtrap()
#else
	#define EMBER_DEBUG_BREAK() __builtin_trap()
#endif

// Tells the compiler that the result or constructed object may borrow storage from this
// parameter. This helps diagnose dangling views, such as a view bound to a temporary. It
// does not extend the storage's lifetime.
//
// Expands to nothing when the attribute is unavailable.
#if EMBER_COMPILER_CLANG
	#define EMBER_LIFETIMEBOUND [[clang::lifetimebound]]
#elif EMBER_COMPILER_MSVC && _MSC_VER >= 1932
	#define EMBER_LIFETIMEBOUND [[msvc::lifetimebound]]
#else
	#define EMBER_LIFETIMEBOUND
#endif

// Cache line size in bytes, used to align and pad independently written data. Sharing a
// cache line can make threads invalidate each other's cached data even when they are
// writing different values.
//
// Defaults to 64. Override this for targets that need a larger separation.
#ifndef EMBER_CACHE_LINE
	#define EMBER_CACHE_LINE 64
#endif

static_assert((EMBER_CACHE_LINE & (EMBER_CACHE_LINE - 1)) == 0,
    "EMBER_CACHE_LINE must be a power of two");

// Keep assertions enabled when EMBER_PROFILE is defined, even with NDEBUG. Profiling
// should still catch invalid state. Other headers use this flag so the assertion policy
// stays in one place.
#if !defined(NDEBUG) || defined(EMBER_PROFILE)
	#define EMBER_ASSERTS_ENABLED 1
#else
	#define EMBER_ASSERTS_ENABLED 0
#endif

namespace Ember
{
	// Reports an assertion failure. Returns true if the caller should break into the
	// debugger. Only [message] may be null.
	//
	// Keeping reporting in one function gives all assertions the same failure path. The
	// default implementation writes to stderr so it does not depend on the engine logger
	// being available.
	[[nodiscard]] bool assert_fail(const char* condition, const char* message,
	    const char* file, int line, const char* function) noexcept;
}

// Checks an internal invariant. Failure indicates a programmer error; use normal error
// handling for invalid input or expected runtime failures.
//
// [expr] must have no side effects because disabled assertions do not evaluate it. With
// assertions enabled, a failed check prevents constant evaluation.
#if EMBER_ASSERTS_ENABLED
	#define EMBER_ASSERT_MSG(expr, message)                                              \
		do                                                                               \
		{                                                                                \
			if (!(expr))                                                                 \
			{                                                                            \
				if (::Ember::assert_fail(#expr, message, __FILE__, __LINE__,             \
				        __FUNCTION__))                                                   \
				{                                                                        \
					EMBER_DEBUG_BREAK();                                                 \
				}                                                                        \
			}                                                                            \
		} while (false)
#else
    // Keep the condition type-checked without evaluating it. Referencing it also avoids
    // unused-variable warnings for values used only by assertions, however rare.
	#define EMBER_ASSERT_MSG(expr, message) ((void)sizeof(!(expr)))
#endif

#define EMBER_ASSERT(expr) EMBER_ASSERT_MSG(expr, nullptr)

// Marks a path that must never execute.
//
// With assertions enabled, reports a failure and aborts, allowing a debugger break first.
// With assertions disabled, tells the optimizer that the path is impossible. Reaching it
// then is undefined behaviour.
#if EMBER_ASSERTS_ENABLED
	#define EMBER_UNREACHABLE()                                                          \
		do                                                                               \
		{                                                                                \
			if (::Ember::assert_fail("unreachable code reached", nullptr, __FILE__,      \
			        __LINE__, __FUNCTION__))                                             \
			{                                                                            \
				EMBER_DEBUG_BREAK();                                                     \
			}                                                                            \
			::std::abort();                                                              \
		} while (false)
#elif EMBER_COMPILER_MSVC
	#define EMBER_UNREACHABLE() __assume(false)
#else
	#define EMBER_UNREACHABLE() __builtin_unreachable()
#endif
