#pragma once

#include "target.h"

namespace Ember
{
	class Material;
	struct DrawCommand
	{
		Target* target = nullptr;
		Material& material;

		DrawCommand(Target* target, Material& material)
			: target(target), material(material) {}

		void submit();
	};
}
