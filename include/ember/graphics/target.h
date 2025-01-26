#pragma once

#include <vector>

#include "graphics/texture.h"
#include "graphics/texture_format.h"
#include "core/common.h"
#include "math/rect.h"

namespace Ember
{
	struct TargetResource;
	class Target
	{
	public:
		Target(u32 width, u32 height);
		Target(u32 width, u32 height, std::initializer_list<TextureFormat> attachments);
		~Target();

		[[nodiscard]] auto size() const {
			return m_rect.size();
		}

		[[nodiscard]] const auto& attachments() const {
			return m_attachments;
		}

	private:
		Rect<u32> m_rect;
		std::vector<Texture> m_attachments;
		Handle<TargetResource> m_resource = Handle<TargetResource>::null;
	};
}
