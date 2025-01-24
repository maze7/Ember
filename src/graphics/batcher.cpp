#include "graphics/batcher.h"

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
	if ((m_batches.size() <= 0 && m_batch.elements <= 0) || m_indices.size() <= 0) {

	}
}
