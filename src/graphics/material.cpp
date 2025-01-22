#include "graphics/material.h"

using namespace Ember;

Material::Material(Shader &shader) : m_shader(shader) {
	set_shader(shader);
}

void Material::set_shader(Shader &shader) {
	m_shader = shader;
	m_vertex_uniform_buffer.resize(m_shader.vertex().uniform_buffer_size());
	m_fragment_uniform_buffer.resize(m_shader.fragment().uniform_buffer_size());
}

void Material::reset() {
	// zero out uniform buffers and samplers
	std::ranges::fill(m_vertex_uniform_buffer, static_cast<std::byte>(0));
	std::ranges::fill(m_fragment_uniform_buffer, static_cast<std::byte>(0));
	m_vertex_samplers.clear();
	m_fragment_samplers.clear();
}

bool Material::has(std::string_view uniform) const {
	return has(uniform, nullptr, nullptr);
}

bool Material::has(std::string_view uniform, UniformType* type, int* array_elements) const {
	auto search_uniform = [&](const auto& uniforms) {
		if (auto it = std::ranges::find_if(uniforms, [&](const auto& u) {
			return u.name == uniform;
		}); it != uniforms.end()) {
			if (type) *type = it->type;
			if (array_elements) *array_elements = it->array_elements;
			return true;
		}

		return false;
	};

	return search_uniform(m_shader.vertex().uniforms) || search_uniform(m_shader.fragment().uniforms);
}

void Material::set(std::string_view uniform, float value) {
	float arr[1] = {value};
	set(uniform, std::as_bytes(std::span(arr)));
}

void Material::set(std::string_view uniform, const glm::vec2& value) {
	float arr[2] = { value.x, value.y };
	set(uniform, std::as_bytes(std::span(arr)));
}

void Material::set(std::string_view uniform, const Color& color) {
	float arr[4] = { (float) color.r / 255, (float) color.g / 255, (float) color.b / 255, (float) color.a / 255 };
	set(uniform, std::as_bytes(std::span(arr)));
}

void Material::set(std::string_view uniform, std::span<const float> values) {
	set(uniform, std::as_bytes(std::span(values)));
}

void Material::set(std::string_view uniform, std::span<const std::byte> data) {
	auto copy_data = [&](const auto& uniforms, std::vector<std::byte>& dst_buffer) {
		size_t offset = 0;
		for (const auto& u : uniforms) {
			const size_t u_size = uniform_size(u.type) * u.array_elements;
			if (u.name == uniform) {
				const size_t copy_size = Math::min(data.size(), u_size);
				std::memcpy(dst_buffer.data() + offset, data.data(), copy_size);
				return true; // stop searching after the first match
			}
			offset += u_size;
		}
		return false;
	};

	copy_data(m_shader.vertex().uniforms, m_vertex_uniform_buffer);
	copy_data(m_shader.fragment().uniforms, m_fragment_uniform_buffer);
}

