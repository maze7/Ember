#pragma once

#include "core/common.h"

namespace Ember
{
	template<class T>
	struct Handle
	{
		u32 slot;
		u32 gen;

		inline static Handle<T> null = { .slot = 0, .gen = 0 };
	};
}