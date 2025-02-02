#include "graphics/batcher.h"
#include "SDL3/SDL.h"
#include <ext/matrix_clip_space.hpp>
#include "graphics/draw_command.h"

using namespace Ember;

// TODO: Move this to a utility method
std::vector<u8> load_file(const char* path) {
	size_t code_size;
	void* code = SDL_LoadFile(path, &code_size);

	if (code != nullptr) {
		std::vector<u8> contents((u8*)code, (u8*) code + code_size);
		SDL_free(code);
		return contents;
	}

	return {};
}

Batcher::Batcher() : m_matrix(1.0f) {
	m_mesh = make_ref<Mesh<Vertex>>();

	m_default_shader = make_ref<Shader>(ShaderDef{
		.vertex = {
			.code = load_file("res/shaders/batcher.vert.spv"),
			.uniforms = {
				{ .name = "matrix", .type = UniformType::Mat4x4 }
			}
		},
		.fragment = {
			.code = load_file("res/shaders/batcher.frag.spv"),
			.num_samplers = 1,
		}
	});
	m_default_material = make_ref<Material>(m_default_shader);

	m_default_sampler = TextureSampler{
		.filter = TextureFilter::Linear,
		.wrap_x = TextureWrap::Repeat,
		.wrap_y = TextureWrap::Repeat,
	};
}

Batcher::~Batcher() {
	m_default_shader.reset();
	m_default_material.reset();
}

void Batcher::new_batch() {
	if (m_batch.has_elements()) {
		u32 offset = m_batch.offset + m_batch.elements;
		m_batches.emplace_back(m_batch);
		m_batch = Batch{};
		m_batch.material = m_default_material;
		m_batch.sampler = m_default_sampler;
		m_batch.texture = nullptr;
		m_batch.offset = offset;
	}
}

void Batcher::clear() {
	m_matrix = glm::mat3x2(1.0);
	m_vertices.clear();
	m_indices.clear();
	m_batches_rendered = 0;

	m_batch = Batch{};
	m_batch.elements = 0;
	m_batch.offset = 0;
	m_batch.material = m_default_material;
	m_batch.texture = nullptr;
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
	for (const auto& batch : m_batches) {
		// render the batch
		render_batch(target, batch, matrix);
	}

	// draw remaining elements in the current batch
	if (m_batch.elements > 0)
		render_batch(target, m_batch, matrix);
}

void Batcher::quad(const Rectf& q, const Ref<Texture>& texture, Color c) {
	quad(
		q.position(),
		{ q.x, q.y + q.h },
		{ q.x + q.w, q.y + q.h },
		{ q.x + q.w, q.y },
		texture,
		c
	);
}

void Batcher::quad(const glm::vec2& v0, const glm::vec2 &v1, const glm::vec2 &v2, const glm::vec2 &v3, const Ref<Texture>& texture, Color c) {
    // precomputed texture coordinates for a quad
    static constexpr glm::vec2 t[4] = {
        {0.0f, 0.0f}, // Top-left
        {0.0f, 1.0f}, // Bottom-left
        {1.0f, 1.0f}, // Bottom-right
        {1.0f, 0.0f}  // Top-right
    };

	quad(v0, v1, v2, v3, t[0], t[1], t[2], t[3], texture, c);
}

void Batcher::quad(const glm::vec2 &v0, const glm::vec2 &v1, const glm::vec2 &v2, const glm::vec2 &v3,
	const glm::vec2 &t0, const glm::vec2 &t1, const glm::vec2 &t2, const glm::vec2 &t3, const Ref<Texture> &texture,
	Color c)
{
	if (texture)
		set_texture(texture);

	u16 start_index = static_cast<u16>(m_vertices.size());

	// add transformed vertices in counter-clockwise order (top-left, bottom-left, bottom-right, top-right)
	m_vertices.push_back({ m_matrix * glm::vec3(v0, 1.0f) , t0, c });
	m_vertices.push_back({ m_matrix * glm::vec3(v1, 1.0f), t1, c });
	m_vertices.push_back({ m_matrix * glm::vec3(v2, 1.0f), t2, c });
	m_vertices.push_back({ m_matrix * glm::vec3(v3, 1.0f), t3, c });


	// add indices for the two triangles form the quad
	static const u16 quad_indices[6] = { 0, 1, 2, 0, 2, 3 };
	for (u16 i : quad_indices)
		m_indices.push_back(start_index + i);

	// update the batch element count
	m_batch.elements += 2; // 2 triangles = 1 quad

	// mark the mesh as dirty
	m_mesh_dirty = true;
}

void Batcher::line(const glm::vec2& from, const glm::vec2& to, float line_width, Color c) {
	set_texture(nullptr);

	auto dir = normalize(to - from);
	auto perp = glm::vec2(-dir.y, dir.x) * line_width * 0.5f;
	quad(from + perp, from - perp, to - perp, to + perp, nullptr, c);
}

void Batcher::push_material(const Ref<Material> &material) {
	// clone the material and push the copy to the stack
	auto copy = make_ref<Material>(*material);
	m_material_stack.push_back(copy);

	// set the current material to the copy
	set_material(copy);
}

void Batcher::pop_material() {
	if (m_material_stack.empty()) {
		set_material(m_default_material);
	} else {
		set_material(m_material_stack.back());
		m_material_stack.pop_back();
	}
}

glm::mat3 Batcher::push_matrix(const glm::mat3& matrix, bool relative) {
	m_matrix_stack.push_back(matrix);

	if (relative)
		m_matrix = matrix * m_matrix;
	else
		m_matrix = matrix;

	return m_matrix;
}

glm::mat3 Batcher::pop_matrix() {
	m_matrix_stack.pop_back();
	m_matrix = m_matrix_stack.empty() ? glm::mat3(1.0) : m_matrix_stack.back();
	return m_matrix;
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

	m_batches_rendered++;
	cmd.submit();
}

void Batcher::set_texture(const Ref<Texture> &texture) {
	// if the current batch has draw data & doesn't use the desired texture, begin a new batch
	if (m_batch.has_elements() && texture != m_batch.texture)
		new_batch();

	// assign the texture to the current batch
	m_batch.texture = texture;
}

void Batcher::set_sampler(const TextureSampler& sampler) {
	// if the current batch has draw data & doesn't use the desired sampler (or the default one), begin a new batch
	if (m_batch.has_elements() && sampler != m_batch.sampler && m_batch.sampler != m_default_sampler)
		new_batch();

	// assign the sampler to the current batch
	m_batch.sampler = sampler;
}

void Batcher::set_material(const Ref<Material>& material) {
	// if the current batch has draw data & doesn't use the desired material, begin a new batch
	if (m_batch.has_elements())
		new_batch();

	// assign the material to the current batch
	m_batch.material = material;
}