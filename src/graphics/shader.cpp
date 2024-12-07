#include "graphics/shader.h"
#include "graphics/render_device.h"

using namespace Ember;

Shader::Shader(const ShaderDef &def) {
	m_resource = render_device->create_shader(def);
}

Shader::~Shader() {
	render_device->destroy_shader(m_resource);
}
