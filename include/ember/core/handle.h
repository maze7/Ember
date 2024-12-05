#pragma once

#include "core/common.h"

namespace Ember
{
	template<class T>
	struct Handle
	{
		u32 slot;
		u32 gen;
	};
}