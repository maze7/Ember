#include "graphics/mesh.h"
#include "graphics/render_device.h"

using namespace Ember;

Mesh::Mesh() {
	m_resource = render_device->create_mesh();
}

Mesh::~Mesh() {
	render_device->destroy_mesh(m_resource);
}