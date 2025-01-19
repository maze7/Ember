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
		Mesh();
		~Mesh();

	private:
		Handle<MeshResource> m_resource = Handle<MeshResource>::null;
	};
}
