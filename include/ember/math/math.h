#pragma once

// GLM types configured for Ember. Include through this header so callers share the same options.

// Projection depth matches Vulkan's [0, 1] range.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

// Require explicit vector conversions to catch accidental component loss.
#define GLM_FORCE_EXPLICIT_CTOR

#include <ember/core/types.h>
#include <ember/math/geometry.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <type_traits> // IWYU pragma: keep

namespace ember
{
	using Vec2 = glm::vec2;
	using Vec3 = glm::vec3;
	using Vec4 = glm::vec4;
	using IVec2 = glm::ivec2;
	using Mat3 = glm::mat3;
	using Mat4 = glm::mat4;
}
