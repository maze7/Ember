#pragma once

#include <vec2.hpp>
#include <vector>

#include "material.h"
#include "mesh.h"

namespace Ember
{
	class Target;
	struct TextureSampler;
	class Texture;
	class Material;
	class Shader;

	class Batcher
	{
	public:
		enum class Modes
		{
			// Renders Textures normally, Multiplied by the Vertex color
			Normal,

			// Renders Textures washed using Vertex Colors, only using the Texture alpha channel.
			Wash,

			// Renders only using Vertex Colors, essentially ignoring the Texture data entirely.
			Fill,
		};

		Batcher();
		~Batcher();

		/**
		 * Clears the Batcher
		 */
		void clear();

		/**
		 * Uploads the current state of the internal Mesh to the GPU
		 */
		void upload();

		/**
		 * Render the Batcher to the provided Target
		 * @param target Ref to Target which should be rendered to, nullptr to render to Swapchain
		 */
		void render(const Ref<Target>& target = nullptr);

	private:
		struct Batch
		{
			Ref<Material> material = nullptr;
			Ref<Texture> texture = nullptr;
			TextureSampler sampler;
			u32 offset = 0;
			u32 elements = 0;
		};

		struct Vertex
		{
			glm::vec2	pos;
			glm::vec2	tex;
			Color		col;

			static VertexFormat format() {
				return VertexFormat({
					{ .index = 0, .type = VertexType::Float2, .normalized = false },
					{ .index = 1, .type = VertexType::Float2, .normalized = false },
					{ .index = 2, .type = VertexType::UByte4, .normalized = true  },
					{ .index = 3, .type = VertexType::UByte4, .normalized = true  },
				});
			}
		};

		void render_batch(const Ref<Target>& target, const Batch& batch, const glm::mat4& matrix);

		Batch						m_batch;
		glm::mat3x2					m_matrix{1.0};
		Mesh<Vertex>				m_mesh;
		std::vector<u16>			m_indices;
		std::vector<Vertex>			m_vertices;
		std::vector<Batch>			m_batches;
		std::vector<glm::mat3x2>	m_matrix_stack;
		std::vector<Ref<Material>>	m_material_stack;
		TextureSampler				m_default_sampler;
		bool						m_mesh_dirty = false;
	};
}
