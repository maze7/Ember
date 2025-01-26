#include "graphics/batcher.h"

#include <ext/matrix_clip_space.hpp>

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
	// render with an orthographic projection matrix to the provided render target
	render(target, glm::ortho(
		0.0f,
		static_cast<float>(target->size().x),
		static_cast<float>(target->size().y),
		0.0f,
		0.0f,
		std::numeric_limits<float>::max()
	));
}

void Batcher::render(const Ref<Target> &target, const glm::mat4 &matrix) {
	if (m_vertices.empty() || m_indices.empty())
		return; // skip render if no vertices have been pushed

	if (m_batches.empty() || m_batch.elements <= 0)
		return; // skip render if no batches have been saved & current batch is empty

	// upload vertex and index data (if the mesh has been modified since the last upload)
	upload();

	// render batches
	for (const auto & m_batche : m_batches) {
		// render the batch
		render_batch(target, m_batche, matrix);
	}

	// draw remaining elements in the current batch
	if (m_batch.elements > 0)
		render_batch(target, m_batch, matrix);
}

void Batcher::render_batch(const Ref<Target>& target, const Batch& batch, const glm::mat4& matrix) {
	batch.material->set("u_matrix", matrix);
	batch.material->set_fragment_sampler(0, batch.texture.get(), batch.sampler);

	DrawCommand cmd = {
		.target = target,
		.material = batch.material,
		.mesh = Ref<Mesh<Vertex>>(&m_mesh),
		.mesh_index_offset = batch.offset * 3,
		.mesh_index_count = batch.elements * 3,
	};

	cmd.submit();
}

void Batcher::set_texture(const Ref<Texture> &texture) {
	// if the current batch has draw data & doesn't use the desired texture, begin a new batch
	if (m_batch.has_elements() && texture != m_batch.texture && m_batch.texture != nullptr) {
		m_batches.emplace_back(m_batch);
		m_batch = Batch{};
	}

	// assign the texture to the current batch
	m_batch.texture = texture;
}

void Batcher::set_sampler(const TextureSampler& sampler) {
	// if the current batch has draw data & doesn't use the desired sampler (or the default one), begin a new batch
	if (m_batch.has_elements() && sampler != m_batch.sampler && m_batch.sampler != m_default_sampler) {
		m_batches.emplace_back(m_batch);
		m_batch = Batch{};
	}

	// assign the sampler to the current batch
	m_batch.sampler = sampler;
}

void Batcher::set_material(const Ref<Material>& material) {
	// if the current batch has draw data & doesn't use the desired material, begin a new batch
	if (m_batch.has_elements()) {
		m_batches.emplace_back(m_batch);
		m_batch = Batch{};
	}

	// assign the material to the current batch
	m_batch.material = material;
}