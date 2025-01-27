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
		static constexpr u32 MAX_SAMPLERS = 32;

		struct BoundSampler
		{
			Texture* texture = nullptr;
			TextureSampler sampler{};
		};

		Material() = default;
		explicit Material(Ref<Shader> shader);

		auto shader() const -> Shader& { return *m_shader; }
		void set_shader(Ref<Shader> shader);

		void reset();
		bool has(std::string_view uniform) const;
		bool has(std::string_view uniform, UniformType* type, int* array_elements) const;

		void set(std::string_view uniform, float value);
		void set(std::string_view uniform, const glm::vec2& value);
		void set(std::string_view uniform, const glm::mat4& value);
		void set(std::string_view uniform, const Color& value);
		void set(std::string_view uniform, std::span<const float> values);
		void set(std::string_view uniform, std::span<const std::byte> data);

		const std::byte* vertex_data() const { return m_vertex_uniform_buffer.data(); }
		const std::byte* fragment_data() const { return m_fragment_uniform_buffer.data(); }

		const auto& vertex_samplers() const { return m_vertex_samplers; }
		const auto& fragment_samplers() const { return m_fragment_samplers; }

		void set_vertex_sampler(u32 index, Texture* texture, const TextureSampler& sampler);
		void set_fragment_sampler(u32 index, Texture* texture, const TextureSampler& sampler);

	private:
		struct UniformInfo
		{
			bool is_vertex = false;
			size_t offset = 0;
			size_t size = 0;
		};

		void build_uniform_lookup();

		Ref<Shader> m_shader = nullptr;
		std::array<BoundSampler, MAX_SAMPLERS> m_vertex_samplers{};
		std::array<BoundSampler, MAX_SAMPLERS> m_fragment_samplers{};
		std::vector<std::byte> m_vertex_uniform_buffer{};
		std::vector<std::byte> m_fragment_uniform_buffer{};
		std::unordered_map<std::string, UniformInfo> m_uniform_lookup;
	};
}
