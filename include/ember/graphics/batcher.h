#pragma once

#include <vec2.hpp>
#include <vector>

#include "material.h"
#include "mesh.h"
#include "math/rect.h"

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

		void new_batch();

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

		/**
		 * Render the Batcher to the provided Target
		 * @param target Render Target to draw the batch to
		 * @param matrix Transform matrix for the whole batch
		 */
		void render(const Ref<Target>& target, const glm::mat4& matrix);

		/**
		 * 
		 * @param quad 
		 * @param c 
		 */
		void quad(const Rect<float>& quad, const Ref<Texture>& texture, Color c);

		/**
		 * Renders a quad to the current Batch
		 * @param v0
		 * @param v1
		 * @param v2
		 * @param v3
		 * @param c
		 */
		void quad(const glm::vec2& v0, const glm::vec2& v1, const glm::vec2& v2, const glm::vec2& v3, const Ref<Texture>& texture = nullptr, Color c = Color::White);

		/**
		 * Draws a line between two points
		 * @param from
		 * @param to
		 * @param line_width
		 * @param c
		 */
		void line(const glm::vec2& from, const glm::vec2& to, float line_width, Color c = Color::White);

		// TODO: Move this to private once the stack management functions are implemented
		void set_texture(const Ref<Texture>& texture);
		void set_sampler(const TextureSampler& sampler);
		void set_material(const Ref<Material>& material);

	private:
		struct Batch
		{
			Ref<Material> material = nullptr;
			Ref<Texture> texture = nullptr;
			TextureSampler sampler;
			u32 offset = 0;
			u32 elements = 0;

			bool has_elements() const { return elements > 0; }
		};

		struct Vertex
		{
			glm::vec2	position;
			glm::vec2	tex_coord;
			Color		color;

			static VertexFormat format() {
				return VertexFormat({
					{ .index = 0, .type = VertexType::Float2, .normalized = false },
					{ .index = 1, .type = VertexType::Float2, .normalized = false },
					{ .index = 2, .type = VertexType::UByte4, .normalized = true  },
				});
			}
		};

		void render_batch(const Ref<Target>& target, const Batch& batch, const glm::mat4& matrix);

		Batch						m_batch;
		glm::mat3x2					m_matrix;
		Ref<Mesh<Vertex>>			m_mesh;
		std::vector<u16>			m_indices;
		std::vector<Vertex>			m_vertices;
		std::vector<Batch>			m_batches;
		std::vector<glm::mat3x2>	m_matrix_stack;
		std::vector<Ref<Material>>	m_material_stack;
		Ref<Material>				m_default_material;
		Ref<Shader>					m_default_shader;
		TextureSampler				m_default_sampler;
		bool						m_mesh_dirty = false;
	};
}
