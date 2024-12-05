#pragma once

#include "core/handle.h"
#include "platform/window.h"
#include "graphics/shader.h"

namespace Ember
{
	class RenderDevice
	{
	public:
		virtual ~RenderDevice() = default;

		virtual void initialize(Window* window) = 0;
		virtual void destroy() = 0;

		virtual Handle<Shader> create_shader(const ShaderDef& def) = 0;
		virtual void destroy_shader(Handle<Shader> shader) = 0;
	};
}
