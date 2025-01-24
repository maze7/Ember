#include "graphics/shader.h"
#include "graphics/render_device.h"

using namespace Ember;

Shader::Shader(const ShaderDef &def) {
	for (const auto& uni0 : def.vertex.uniforms) {
		for (const auto& uni1 : def.fragment.uniforms) {
			if (uni0.name == uni1.name && (uni0.type != uni1.type || uni0.array_elements != uni1.array_elements))
				throw Exception("Uniform names must be unique between Vertex and Fragment shaders, or they must be matching types.");
		}
	}

	m_resource = render_device->create_shader(def);

	m_vertex = Program{
		.num_samplers = def.vertex.num_samplers,
		.uniforms = def.vertex.uniforms,
	};

	m_fragment = Program{
		.num_samplers = def.fragment.num_samplers,
		.uniforms = def.fragment.uniforms,
	};
}

Shader::~Shader() {
	render_device->dispose_shader(m_resource);
}
