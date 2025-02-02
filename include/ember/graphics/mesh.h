#pragma once

#include <span>

#include "index_format.h"
#include "vertex_format.h"
#include "core/common.h"
#include "core/handle.h"

namespace Ember
{
	struct MeshResource;

	class VoidMesh
	{
	public:
		VoidMesh();
		VoidMesh(VertexFormat vertex_format, IndexFormat index_format);
		~VoidMesh();

		void set_vertices(const void* data, int count, int offset = 0);
		void set_indices(const void* data, int count, int offset = 0);
		auto resource() const { return m_resource; }
		auto& vertex_format() const { return m_vertex_format; }
		auto index_format() const { return m_index_format; }

	private:
		Handle<MeshResource> m_resource = Handle<MeshResource>::null;
		VertexFormat m_vertex_format;
		IndexFormat m_index_format;

		u32 m_vertex_count = 0;
		u32 m_index_count = 0;
	};

	// C++20 concept to check that a type T has a static function 'format() -> VertexFormat'
	template <class T>
	concept VertexConcept = requires {
		{ T::format() } -> std::same_as<VertexFormat>;
	};

	// Type-specialised Mesh class that wraps VoidMesh
	template <VertexConcept TVertex, class TIndex = u16>
	class Mesh : public VoidMesh
	{
	public:
		// Construct by deriving the correct vertex & index formats
		Mesh() : VoidMesh(TVertex::format(), index_format_from_type<TIndex>()) {}

		void set_vertices(std::span<const TVertex> data, int offset = 0) {
			VoidMesh::set_vertices(data.data(), data.size(), offset);
		}

		void set_indices(std::span<const TIndex> data, int offset = 0) {
			VoidMesh::set_indices(data.data(), data.size(), offset);
		}
	};
}