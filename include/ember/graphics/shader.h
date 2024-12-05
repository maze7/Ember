#pragma once

#include <vector>

#include "graphics.h"
#include "core/common.h"

namespace Ember
{
	struct ShaderDef
	{
		struct VertexAttribute
		{
			u64 offset = 0;
			Format format = Format::Float;
		};

		struct VertexBinding
		{
			u32 stride = 0;
			std::vector<VertexAttribute> attributes;
		};

		const char*					debug_name;
		std::vector<u8>				vertex;
		std::vector<u8>				fragment;
		std::vector<VertexBinding>	bindings;
	};

	class Shader {};
}