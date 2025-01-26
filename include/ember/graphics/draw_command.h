#pragma once

#include "mesh.h"
#include "target.h"

namespace Ember
{
	class Material;
	struct DrawCommand
	{
		Ref<Target> target = nullptr;
		Ref<Material> material = nullptr;
		Ref<VoidMesh> mesh = nullptr;

		u32 mesh_index_offset = 0;
		u32 mesh_index_count = 0;
		u32 mesh_vertex_offset = 0;

		void submit();
	};
}
