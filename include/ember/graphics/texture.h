#pragma once

#include "render_device.h"
#include "texture_format.h"
#include "core/common.h"
#include "math/vector2.h"

namespace Ember
{
	class Texture
	{
	public:

		Texture(u32 width, u32 height, TextureFormat format = TextureFormat::Color);
		~Texture();

	private:
		bool m_is_target_attachment = false;
		Vector2u m_size = Vector2u::zero;
		TextureFormat m_format = TextureFormat::Color;
		TextureHandle m_resource = Handle<TextureResource>::null;
	};
}
