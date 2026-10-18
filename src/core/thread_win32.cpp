#if defined(_WIN32)

	#include <ember/core/macros.h>
	#include <ember/core/profile.h>
	#include <ember/core/thread.h>

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>

namespace ember::thread
{
	void set_name(const char* name)
	{
		EMBER_ASSERT(name != nullptr);

		// SetThreadDescription is looked up rather than linked: it arrived in Windows 10 1607,
		// and older SDK headers, MinGW's among them, do not declare it. The debugger and crash
		// dumps both show what it sets. Going through void (*)() keeps GCC from warning about
		// the cast between function types.
		using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
		static const SetDescription s_set_description = (SetDescription)(void (*)())GetProcAddress(
		    GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");

		if (s_set_description != nullptr)
		{
			// Names are ASCII, so widening is a copy.
			wchar_t wide[64];
			size_t length = 0;
			while (name[length] != '\0' && length < 63)
			{
				wide[length] = (wchar_t)name[length];
				++length;
			}

			wide[length] = L'\0';
			(void)s_set_description(GetCurrentThread(), wide);
		}

		EMBER_PROFILE_THREAD(name);
	}

	bool set_affinity(u32 processor)
	{
		// A machine with more than sixty-four logical processors splits them into groups, and a
		// thread is pinned within one group. The groups' active processors are counted in
		// order, so the flat number falls in exactly one of them.
		const WORD groups = GetActiveProcessorGroupCount();
		u32 first = 0;

		for (WORD group = 0; group < groups; ++group)
		{
			const u32 count = GetActiveProcessorCount(group);
			if (processor < first + count)
			{
				GROUP_AFFINITY affinity = {};
				affinity.Group = group;
				affinity.Mask = (KAFFINITY)1 << (processor - first);
				return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
			}

			first += count;
		}

		return false;
	}

	u32 hardware_count()
	{
		static const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
		return count > 0 ? (u32)count : 1;
	}

	void sleep(u64 ns)
	{
		Sleep((DWORD)((ns + 999'999) / 1'000'000));
	}
}

#endif
