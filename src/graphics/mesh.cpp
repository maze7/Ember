#include "graphics/mesh.h"
#include "graphics/render_device.h"

using namespace Ember;

VoidMesh::VoidMesh() {}

VoidMesh::VoidMesh(VertexFormat vertex_format, IndexFormat index_format)
	: m_vertex_format(vertex_format), m_index_format(index_format)
{
}

VoidMesh::~VoidMesh() {
	if (!m_resource.is_null()) {
		render_device->destroy_mesh(m_resource);
	}
}

void VoidMesh::set_vertices(void *data, int count, int offset) {
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

void VoidMesh::set_indices(void* data, int count, int offset) {
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
