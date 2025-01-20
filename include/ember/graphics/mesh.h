#pragma once
#include "index_format.h"
#include "vertex_format.h"
#include "core/common.h"
#include "core/handle.h"

namespace Ember
{
	struct MeshResource;

	class Mesh
	{
	public:
		Mesh(VertexFormat vertex_format, IndexFormat index_format);
		~Mesh();

		void set_vertices(void* data, int count, int offset = 0);

	private:
		Handle<MeshResource> m_resource = Handle<MeshResource>::null;
		VertexFormat m_vertex_format;
		IndexFormat m_index_format;

		u32 m_vertex_count = 0;
		u32 m_index_count = 0;
	};
}
