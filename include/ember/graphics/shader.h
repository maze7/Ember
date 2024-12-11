#pragma once

#include <vector>

#include "render_device.h"
#include "core/common.h"

namespace Ember
{
	struct ShaderDef
	{
		std::vector<u8> vertex;
		std::vector<u8> fragment;
	};

	class Shader
	{
	public:
		explicit Shader(const ShaderDef& def);
		~Shader();

	private:
		Handle<ShaderResource> m_resource = Handle<ShaderResource>::null;
	};
}