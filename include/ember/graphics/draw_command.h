#pragma once

#include "mesh.h"
#include "target.h"

namespace Ember
{
	class Material;
	struct DrawCommand
	{
		Target* target = nullptr;
		Material& material;
		VoidMesh* mesh = nullptr;

		u32 mesh_index_start = 0;
		u32 mesh_index_count = 0;
		u32 mesh_vertex_offset = 0;

		DrawCommand(Target* target, Material& material, VoidMesh* mesh = nullptr)
			: target(target), material(material), mesh(mesh) {}

		void submit();
	};
}
