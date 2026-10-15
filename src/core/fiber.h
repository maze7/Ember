#pragma once

// A fiber is a stack and the registers needed to resume code on it. A switch runs entirely on the
// calling thread and saves only the callee-saved registers, the stack pointer and the return
// address, so a job can park in the middle of a function and the worker moves on to other work
// within nanoseconds. The switch is hand-written assembly on x86-64 and AArch64. On Windows it is
// OS fibers, which bring their own stacks, and the stack handed in is ignored.
//
// Contracts:
// 	- The entry never returns. A fiber ends by switching away for the last time, and its owner keeps
// 	  the stack.
//  - A fiber runs on at most one thread at a time and is never switched to while it runs.
//  - An adopted thread's fiber holds no state until the first switch away from it, so until then it
// 	  can only be a switch source.
//  - A thread local read before a switch may be wrong after it. The compiler may cache the address
//    for the whole function, and the funciton may finish on another thread.

#include <cstddef>
#include <ember/core/types.h>

namespace ember::jobs
{
	using FiberEntry = void (*)(void* arg);

	// The saved registers, laid out by the switch code for the architecture. See fiber_posix.cpp.
	// Sized for the largest layout  and aligned for the vector registers.
	struct alignas(16) FiberContext
	{
		u8 bytes[192];
	};

	struct FiberDef
	{
		// The stack, lowest address first, from the stack partition as a rule. The fiber never
		// frees it, and a guard page below it is the owners to arrange.
		void* stack = nullptr;
		size_t stack_size = 0;

		FiberEntry entry = nullptr;
		void* arg = nullptr;
	};

	// Everything a fiber is, in the open so that a record holds one by value and nothing is
	// allocated. Only the fiber functions write it.
	struct Fiber
	{
		FiberContext context;
		void* stack = nullptr;
		size_t stack_size = 0;
		FiberEntry entry = nullptr;
		void* arg = nullptr;

		// The Win32 fiber handle, or ThreadSanitizer's view of the fiber.
		void* platform = nullptr;

		// What an AddressSanitizer needs across a switch: the fake stack it keeps for this fiber,
		// and the bounds of the real one.
		void* fake_stack = nullptr;
		const void* stack_bottom = nullptr;
		size_t stack_bytes = 0;

		bool adopted = false;
	};

	// Prepares [fiber] to run [def.entry] with [def.arg] on [def.stack] the first time something
	// switches to it.
	void fiber_initialize(Fiber& fiber, const FiberDef& def);

	// Undoes fiber_initialize: no-op on POSIX, the system's fiber on Windows. Never the running
	// fiber.
	void fiber_destroy(Fiber& fiber);

	// Wraps the calling thread's own stack so that it can take part in switches, and undoes that.
	// Only the thread itself, and release only once the thread is back on its own stack.
	void fiber_adopt_thread(Fiber& fiber);
	void fiber_release_thread(Fiber& fiber);

	// Saves the running state into [from] and resumes [to]. Returns once something switches back
	// to [from], on whatever thread does so.
	void fiber_switch(Fiber& from, Fiber& to);
}
