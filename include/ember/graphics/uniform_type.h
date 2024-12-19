#pragma once
#include "core/common.h"

namespace Ember
{
	enum class UniformType
	{
		None,
		Float,
		Float2,
		Float3,
		Float4,
		Mat3x2,
		Mat4x4,
	};

	constexpr static u32 uniform_size(UniformType type) {
		switch (type) {
			case UniformType::None:
				return 0;
			case UniformType::Float:
				return 4;
			case UniformType::Float2:
				return 8;
			case UniformType::Float3:
				return 12;
			case UniformType::Float4:
				return 16;
			case UniformType::Mat3x2:
				return 24;
			case UniformType::Mat4x4:
				return 64;
			default:
				throw Exception("Unknown uniform type.");
		}
	}
}
