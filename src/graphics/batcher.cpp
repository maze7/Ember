#include "graphics/batcher.h"

#include "graphics/draw_command.h"

using namespace Ember;

Batcher::Batcher() {

}

Batcher::~Batcher() {

}

void Batcher::clear() {
	m_matrix = glm::mat3x2(1.0);
	m_vertices.clear();
	m_indices.clear();

	m_batch.elements = 0;
	m_batch.offset = 0;
	m_batch.material.reset();
	m_batch.texture.reset();
	m_batch.sampler = m_default_sampler;

	m_matrix_stack.clear();
	m_material_stack.clear();
	m_batches.clear();
}

void Batcher::upload() {
	if (m_mesh_dirty && m_vertices.size() >= 0 && m_indices.size() >= 0) {
		m_mesh.set_vertices(m_vertices);
		m_mesh.set_indices(m_indices);
		m_mesh_dirty = false;
	}
}

void Batcher::render(const Ref<Target> &target) {

}

void Batcher::render_batch(const Ref<Target>& target, const Batch& batch, const glm::mat4& matrix) {
	batch.material->set("u_matrix", matrix);
}




