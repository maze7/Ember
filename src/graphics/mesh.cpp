#include "graphics/mesh.h"
#include "graphics/render_device.h"

using namespace Ember;

Mesh::Mesh() {
	m_resource = render_device->create_mesh();
}

Mesh::Mesh(VertexFormat vertex_format, IndexFormat index_format)
	: m_vertex_format(vertex_format), m_index_format(index_format)
{
}

Mesh::~Mesh() {
	if (!m_resource.is_null()) {
		render_device->destroy_mesh(m_resource);
	}
}

void Mesh::set_vertices(void *data, int count, int offset) {
	m_vertex_count = count;

	if (m_resource.is_null()) {
		m_resource = render_device->create_mesh();
	}

	render_device->set_mesh_vertex_data(
		m_resource,
		data,
		(int) m_vertex_format.stride * count,
		(int) m_vertex_format.stride * offset
	);
}

void Mesh::set_indices(void* data, int count, int offset = 0) {
	m_index_count = count;

	if (m_resource.is_null()) {
		m_resource = render_device->create_mesh();
	}

	render_device->set_mesh_index_data(
		m_resource,
		data,
		(int) IndexFormatExt::size_in_bytes(m_index_format) * count,
		(int) IndexFormatExt::size_in_bytes(m_index_format) * offset
	);
}
