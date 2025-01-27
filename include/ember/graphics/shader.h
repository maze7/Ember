#pragma once

#include <vector>
#include <numeric>

#include "render_device.h"
#include "uniform_type.h"
#include "core/common.h"

namespace Ember
{
	struct ShaderUniform
	{
		std::string name;
		UniformType type;
		int array_elements = 1;
	};

	struct ShaderProgramDef
	{
		std::vector<u8> code{};
		u32 num_samplers = 0;
		std::vector<ShaderUniform> uniforms{};
		const char* entry_point = "main";
	};

	struct ShaderDef
	{
		ShaderProgramDef vertex;
		ShaderProgramDef fragment;
	};

	class Shader
	{
	public:
		struct Program
		{
			u32 num_samplers = 0;
			std::vector<ShaderUniform> uniforms;

			[[nodiscard]] u32 uniform_buffer_size() const {
				return std::accumulate(uniforms.begin(), uniforms.end(), 0u, [](u32 sum, const auto& u) {
					return sum + uniform_size(u.type);
				});
			}
		};

		Shader(const ShaderDef& def);
		~Shader();

		[[nodiscard]] const Program& vertex() const { return m_vertex; }
		[[nodiscard]] const Program& fragment() const { return m_fragment; }

		bool operator==(const Shader& rhs) const {
			return m_resource == rhs.m_resource;
		}

		auto handle() const { return m_resource; }

	private:
		Program m_vertex;
		Program m_fragment;
		Handle<ShaderResource> m_resource = Handle<ShaderResource>::null;
	};
}