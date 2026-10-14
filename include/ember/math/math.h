#pragma once

// The engine's vector and matrix types, which are glm's behind Ember names. This header is intended
// to be the one place glm is included.

// Makes GLM's projection functions produce Vulkan's [0, 1] depth range instead of OpenGL's [-1, 1]
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

// Stops GLM converting between vector sizes implicitly, so passing a Vec4 where a Vec3 is expected
// is a compile error rather than truncation.
#define GLM_FORCE_EXPLICIT_CTOR

#include <ember/core/types.h>
#include <ember/math/geometry.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <type_traits>

namespace Ember
{
	using Vec2 = glm::vec2;
	using Vec3 = glm::vec3;
	using Vec4 = glm::vec4;
	using IVec2 = glm::ivec2;
	using Mat3 = glm::mat3;
	using Mat4 = glm::mat4;
}
