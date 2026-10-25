#pragma once

// Fibers suspend a job on its stack while the worker runs other work. POSIX switches save
// register state in assembly; Windows fibers own their stacks and ignore the supplied stack.
//
// Entries never return; a finished fiber switches away and leaves its stack with the owner.
// A fiber must run on only one thread at a time. An adopted thread can be a switch destination
// only after its first switch away has saved its context.
//
// A switch may resume on another thread. Read thread locals through non-inlined functions so
// the compiler cannot reuse a thread-local address captured before the switch.

#include <cstddef>
#include <ember/core/types.h>

namespace ember::jobs
{
	using FiberEntry = void (*)(void* arg);

	// Storage for either architecture's register layout, aligned for vector registers.
	// Offsets are checked in fiber_posix.cpp against the assembly layout.
	struct alignas(16) FiberContext
	{
		u8 bytes[192];
	};

	struct FiberDef
	{
		// Borrowed stack, lowest address first. The owner arranges the guard page and frees it
		// only after the fiber can no longer run.
		void* stack = nullptr;
		size_t stack_size = 0;

		FiberEntry entry = nullptr;
		void* arg = nullptr;
	};

	// Stored by value in scheduler records. Only the fiber functions may modify this state.
	struct Fiber
	{
		FiberContext context;
		void* stack = nullptr;
		size_t stack_size = 0;
		FiberEntry entry = nullptr;
		void* arg = nullptr;

		// The Win32 fiber handle, or ThreadSanitizer's view of the fiber.
		void* platform = nullptr;

		// AddressSanitizer needs both its fake stack and the real stack bounds across switches.
		void* fake_stack = nullptr;
		const void* stack_bottom = nullptr;
		size_t stack_bytes = 0;

		bool adopted = false;
	};

	void fiber_initialize(Fiber& fiber, const FiberDef& def);

	// Releases platform state, not the supplied stack. The fiber must not be running.
	void fiber_destroy(Fiber& fiber);

	// Adopt and release only on the owning thread. Release after it returns to its own stack.
	void fiber_adopt_thread(Fiber& fiber);
	void fiber_release_thread(Fiber& fiber);

	// Saves the running state into [from] and resumes [to]. Returns once something switches back
	// to [from], on whatever thread does so.
	void fiber_switch(Fiber& from, Fiber& to);
}
