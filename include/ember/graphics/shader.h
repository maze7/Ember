#pragma once

#include <vector>
#include "core/common.h"

namespace Ember
{
	struct ShaderDef
	{
		std::vector<u8> vertex;
		std::vector<u8> fragment;
	};
}