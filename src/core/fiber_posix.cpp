// POSIX fibers: the register state lives in the Fiber and the switch code in
// fiber_x86_64_sysv.S or fiber_aarch64_aapcs.S moves between contexts. The stack is the
// owner's, usually a slice of the stack partition with a guard page below it.

#include <ember/core/macros.h>
#include <ember/memory/alignment.h>

#include "fiber.h"

#include <cstddef>
#include <cstdint>

#if defined(__SANITIZE_THREAD__)
	#define EMBER_TSAN 1
#elif defined(__has_feature)
	#if __has_feature(thread_sanitizer)
		#define EMBER_TSAN 1
	#endif
#endif

#if defined(EMBER_TSAN)
	#include <sanitizer/tsan_interface.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
	#define EMBER_ASAN 1
#elif defined(__has_feature)
	#if __has_feature(address_sanitizer)
		#define EMBER_ASAN 1
	#endif
#endif

#if defined(EMBER_ASAN)
	#include <pthread.h>
	#include <sanitizer/common_interface_defs.h>
#endif

#if defined(__x86_64__)
	#include <xmmintrin.h>
#endif

namespace ember::jobs
{
	namespace
	{
#if defined(__x86_64__)
		// The saved register set. Offsets are shared with fiber_x86_64_sysv.S.
		struct alignas(16) Registers
		{
			u64 rsp = 0;
			u64 rip = 0;
			u64 rbx = 0;
			u64 rbp = 0;
			u64 r12 = 0;
			u64 r13 = 0;
			u64 r14 = 0;
			u64 r15 = 0;
			u32 mxcsr = 0;
			u16 x87_control = 0;
		};

		static_assert(offsetof(Registers, rsp) == 0);
		static_assert(offsetof(Registers, rip) == 8);
		static_assert(offsetof(Registers, rbx) == 16);
		static_assert(offsetof(Registers, rbp) == 24);
		static_assert(offsetof(Registers, r12) == 32);
		static_assert(offsetof(Registers, r13) == 40);
		static_assert(offsetof(Registers, r14) == 48);
		static_assert(offsetof(Registers, r15) == 56);
		static_assert(offsetof(Registers, mxcsr) == 64);
		static_assert(offsetof(Registers, x87_control) == 68);
		static_assert(sizeof(Registers) == 80);
#elif defined(__aarch64__)
		// The saved register set. Offsets are shared with fiber_aarch64_aapcs.S.
		struct alignas(16) Registers
		{
			u64 sp = 0;
			u64 lr = 0;
			u64 x19 = 0;
			u64 x20 = 0;
			u64 x21 = 0;
			u64 x22 = 0;
			u64 x23 = 0;
			u64 x24 = 0;
			u64 x25 = 0;
			u64 x26 = 0;
			u64 x27 = 0;
			u64 x28 = 0;
			u64 x29 = 0;
			u64 d8 = 0;
			u64 d9 = 0;
			u64 d10 = 0;
			u64 d11 = 0;
			u64 d12 = 0;
			u64 d13 = 0;
			u64 d14 = 0;
			u64 d15 = 0;
			u64 fpcr = 0;
		};

		static_assert(offsetof(Registers, sp) == 0);
		static_assert(offsetof(Registers, lr) == 8);
		static_assert(offsetof(Registers, x19) == 16);
		static_assert(offsetof(Registers, x28) == 88);
		static_assert(offsetof(Registers, x29) == 96);
		static_assert(offsetof(Registers, d8) == 104);
		static_assert(offsetof(Registers, d15) == 160);
		static_assert(offsetof(Registers, fpcr) == 168);
		static_assert(sizeof(Registers) == 176);
#else
	#error "Unsupported POSIX fiber architecture"
#endif

		static_assert(sizeof(Registers) <= sizeof(FiberContext) &&
		                  alignof(Registers) <= alignof(FiberContext),
		    "FiberContext must hold the architecture's registers");

		Registers& registers_of(Fiber& fiber)
		{
			return *reinterpret_cast<Registers*>(fiber.context.bytes);
		}
	}
}

extern "C"
{
	void ember_fiber_switch(void* from, void* to);
	void ember_fiber_trampoline();
}

namespace ember::jobs
{
	namespace
	{
		// The first code on a fresh stack. Lets the sanitizer finish the switch before user code
		// runs, then calls the entry, which never returns.
		void fiber_entry(void* arg)
		{
			Fiber* fiber = (Fiber*)arg;
#if defined(EMBER_ASAN)
			__sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
			fiber->entry(fiber->arg);
		}

#if defined(EMBER_ASAN)
		void query_thread_stack(const void*& bottom, size_t& size)
		{
	#if defined(__APPLE__)
			const pthread_t self = pthread_self();
			size = pthread_get_stacksize_np(self);
			bottom = (const u8*)pthread_get_stackaddr_np(self) - size;
	#else
			pthread_attr_t attributes;
			void* address = nullptr;
			pthread_getattr_np(pthread_self(), &attributes);
			pthread_attr_getstack(&attributes, &address, &size);
			pthread_attr_destroy(&attributes);
			bottom = address;
	#endif
		}
#endif

#if defined(__x86_64__)
		u16 read_x87_control()
		{
			u16 control = 0;
			__asm__ __volatile__("fnstcw %0" : "=m"(control));
			return control;
		}

		void init_registers(Registers& registers, uintptr_t stack_top, FiberEntry entry, void* arg)
		{
			registers.rsp = align_down(stack_top, uintptr_t{16});
			registers.rip = (u64)(uintptr_t)&ember_fiber_trampoline;
			registers.r12 = (u64)(uintptr_t)arg;
			registers.r13 = (u64)(uintptr_t)entry;
			registers.mxcsr = _mm_getcsr();
			registers.x87_control = read_x87_control();
		}
#elif defined(__aarch64__)
		u64 read_fpcr()
		{
			u64 value = 0;
			__asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
			return value;
		}

		void init_registers(Registers& registers, uintptr_t stack_top, FiberEntry entry, void* arg)
		{
			registers.sp = align_down(stack_top, uintptr_t{16});
			registers.lr = (u64)(uintptr_t)&ember_fiber_trampoline;
			registers.x19 = (u64)(uintptr_t)arg;
			registers.x20 = (u64)(uintptr_t)entry;
			registers.fpcr = read_fpcr();
		}
#endif
	}

	void fiber_initialize(Fiber& fiber, const FiberDef& def)
	{
		EMBER_ASSERT(def.entry != nullptr);
		EMBER_ASSERT(def.stack != nullptr && def.stack_size != 0);

		fiber = Fiber{};
		fiber.stack = def.stack;
		fiber.stack_size = def.stack_size;
		fiber.entry = def.entry;
		fiber.arg = def.arg;

		Registers& registers = registers_of(fiber);
		registers = Registers{};
		init_registers(registers, (uintptr_t)def.stack + def.stack_size, fiber_entry, &fiber);

#if defined(EMBER_TSAN)
		fiber.platform = __tsan_create_fiber(0);
#endif
#if defined(EMBER_ASAN)
		fiber.stack_bottom = def.stack;
		fiber.stack_bytes = def.stack_size;
#endif
	}

	void fiber_destroy(Fiber& fiber)
	{
		EMBER_ASSERT(!fiber.adopted);

#if defined(EMBER_TSAN)
		if (fiber.platform != nullptr)
		{
			__tsan_destroy_fiber(fiber.platform);
		}
#endif

		fiber = Fiber{};
	}

	void fiber_adopt_thread(Fiber& fiber)
	{
		fiber = Fiber{};
		fiber.adopted = true;

#if defined(EMBER_TSAN)
		fiber.platform = __tsan_get_current_fiber();
#endif
#if defined(EMBER_ASAN)
		query_thread_stack(fiber.stack_bottom, fiber.stack_bytes);
#endif
	}

	void fiber_release_thread(Fiber& fiber)
	{
		EMBER_ASSERT(fiber.adopted);
		fiber = Fiber{};
	}

	void fiber_switch(Fiber& from, Fiber& to)
	{
		EMBER_ASSERT(&from != &to);

#if defined(EMBER_ASAN)
		__sanitizer_start_switch_fiber(&from.fake_stack, to.stack_bottom, to.stack_bytes);
#endif
#if defined(EMBER_TSAN)
		__tsan_switch_to_fiber(to.platform, 0);
#endif

		ember_fiber_switch(from.context.bytes, to.context.bytes);

#if defined(EMBER_ASAN)
		__sanitizer_finish_switch_fiber(from.fake_stack, nullptr, nullptr);
#endif
	}
}
