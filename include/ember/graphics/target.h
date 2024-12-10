#pragma once

#include <span>
#include <vector>

#include "texture.h"
#include "core/common.h"
#include "math/rect.h"

namespace Ember
{
	class Target
	{
	public:
		Target(u32 width, u32 height);
		Target(u32 width, u32 height, std::span<TextureFormat> attachments);
		~Target();

		[[nodiscard]] auto size() const {
			return m_rect.size();
		}

		[[nodiscard]] const auto& attachments() const {
			return m_attachments;
		}

		operator Handle<Target> () const { return m_resource; }

	private:
		Rect<u32> m_rect;
		std::vector<Texture> m_attachments;
		Handle<Target> m_resource = Handle<Target>::null;
	};
}
