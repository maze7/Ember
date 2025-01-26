#include "graphics/batcher.h"

#include <ext/matrix_clip_space.hpp>

#include "graphics/draw_command.h"

using namespace Ember;

Batcher::Batcher() : m_matrix(1.0f) {
	m_mesh = make_ref<Mesh<Vertex>>();
}

Batcher::~Batcher() {

}

void Batcher::new_batch() {
	if (m_batch.has_elements()) {
		m_batches.emplace_back(m_batch);
		m_batch = Batch{};
	}
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
		m_mesh->set_vertices(m_vertices);
		m_mesh->set_indices(m_indices);
		m_mesh_dirty = false;
	}
}

void Batcher::render(const Ref<Target> &target) {
	auto render_target = target ? target : render_device->framebuffer();

	// render with an orthographic projection matrix to the provided render target
	render(render_target, glm::ortho(
		0.0f,
		static_cast<float>(render_target->size().x),
		static_cast<float>(render_target->size().y),
		0.0f,
		0.0f,
		std::numeric_limits<float>::max()
	));
}

void Batcher::render(const Ref<Target> &target, const glm::mat4 &matrix) {
	if (m_vertices.empty() || m_indices.empty())
		return; // skip render if no vertices have been pushed

	if (m_batches.empty() && m_batch.elements <= 0)
		return; // skip render if no batches have been saved & current batch is empty

	// upload vertex and index data (if the mesh has been modified since the last upload)
	upload();

	// render batches
	for (const auto& m_batch : m_batches) {
		// render the batch
		render_batch(target, m_batch, matrix);
	}

	// draw remaining elements in the current batch
	if (m_batch.elements > 0)
		render_batch(target, m_batch, matrix);
}

void Batcher::quad(const Rect<float>& q, const Ref<Texture>& texture, Color c) {
	quad(
		q.position(),
		{ q.x, q.y + q.height },
		{ q.x + q.width, q.y + q.height },
		{ q.x + q.width, q.y },
		texture,
		c
	);
}

void Batcher::quad(const glm::vec2& v0, const glm::vec2 &v1, const glm::vec2 &v2, const glm::vec2 &v3, const Ref<Texture>& texture, Color c) {
	if (texture)
		set_texture(texture);

    // Reserve memory upfront to avoid multiple reallocations
    if (m_vertices.capacity() - m_vertices.size() < 4) {
        m_vertices.reserve(m_vertices.size() * 2);
    }
    if (m_indices.capacity() - m_indices.size() < 6) {
        m_indices.reserve(m_indices.size() * 2);
    }

    // Precomputed texture coordinates for a quad
    static constexpr glm::vec2 tex_coords[4] = {
        {0.0f, 0.0f}, // Top-left
        {0.0f, 1.0f}, // Bottom-left
        {1.0f, 1.0f}, // Bottom-right
        {1.0f, 0.0f}  // Top-right
    };

    // Compute the starting index for this quad
    u16 startIndex = static_cast<u16>(m_vertices.size());

    // Add transformed vertices in clockwise order (top-left -> bottom-left -> bottom-right -> top-right)
    m_vertices.push_back({m_matrix * glm::vec3(v0, 1.0f), tex_coords[0], c});
    m_vertices.push_back({m_matrix * glm::vec3(v1, 1.0f), tex_coords[1], c});
    m_vertices.push_back({m_matrix * glm::vec3(v2, 1.0f), tex_coords[2], c});
    m_vertices.push_back({m_matrix * glm::vec3(v3, 1.0f), tex_coords[3], c});

    // Add indices for the two triangles forming the quad
    static const u16 quadIndices[6] = {0, 1, 2, 0, 2, 3};
    for (u16 i : quadIndices) {
        m_indices.push_back(startIndex + i);
    }

    // Update the batch element count
    m_batch.elements += 2; // 2 triangles = 1 quad

    // Mark the mesh as dirty
    m_mesh_dirty = true;
}

void Batcher::line(const glm::vec2& from, const glm::vec2& to, float line_width, Color c) {
	auto dir = normalize(to - from);
	auto perp = glm::vec2(-dir.y, dir.x) * line_width * 0.5f;
	quad(from + perp, from - perp, to - perp, to + perp, nullptr, c);
}

void Batcher::render_batch(const Ref<Target>& target, const Batch& batch, const glm::mat4& matrix) {
	batch.material->set("matrix", matrix);
	batch.material->set_fragment_sampler(0, batch.texture.get(), batch.sampler);

	DrawCommand cmd = {
		.target = target,
		.material = batch.material,
		.mesh = m_mesh,
		.mesh_index_offset = batch.offset * 3,
		.mesh_index_count = batch.elements * 3,
	};

	cmd.submit();
}

void Batcher::set_texture(const Ref<Texture> &texture) {
	// if the current batch has draw data & doesn't use the desired texture, begin a new batch
	if (m_batch.has_elements() && texture != m_batch.texture) {
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