#pragma once

#include <span>

#include "shader.h"
#include "texture.h"
#include "core/common.h"

namespace Ember
{
	class Material
	{
	public:
		struct BoundSampler
		{
			Texture* texture = nullptr;
			TextureSampler sampler{};
		};

		explicit Material(Shader& shader);

		auto shader() const -> Shader& { return m_shader; }
		void set_shader(Shader& shader);

		void reset();
		bool has(std::string_view uniform) const;
		bool has(std::string_view uniform, UniformType* type, int* array_elements) const;

		void set(std::string_view uniform, float value);
		void set(std::string_view uniform, const Vector2f& value);
		void set(std::string_view uniform, const Color& value);
		void set(std::string_view uniform, std::span<const float> values);
		void set(std::string_view uniform, std::span<const std::byte> data);

		const std::byte* vertex_data() const { return m_vertex_uniform_buffer.data(); }
		const std::byte* fragment_data() const { return m_fragment_uniform_buffer.data(); }

	private:
		struct UniformInfo
		{
			bool is_vertex;
			size_t offset;
			size_t size;
		};

		void build_uniform_lookup();

		Shader& m_shader;
		std::vector<BoundSampler> m_vertex_samplers;
		std::vector<BoundSampler> m_fragment_samplers;
		std::vector<std::byte> m_vertex_uniform_buffer;
		std::vector<std::byte> m_fragment_uniform_buffer;
		std::unordered_map<std::string, UniformInfo> m_uniform_lookup;
	};
}
