#pragma once

#include "graphics/render_device.h"
#include "graphics/texture.h"
#include "core/common.h"
#include "math/vector2.h"

namespace Ember
{
	enum class TextureFilter
	{
		Nearest,
		Linear,
	};

	enum class TextureWrap
	{
		Repeat,
		MirroredRepeat,
		Clamp,
	};

	struct TextureSampler
	{
		TextureFilter filter;
		TextureWrap wrap_x;
		TextureWrap wrap_y;
	};

	class Texture
	{
	public:

		Texture(u32 width, u32 height, TextureFormat format = TextureFormat::Color, Target* target = nullptr);
		~Texture();

		Handle<TextureResource> handle() const { return m_resource; }

	private:
		bool m_is_target_attachment = false;
		Vector2u m_size = Vector2u::zero;
		TextureFormat m_format = TextureFormat::Color;
		Handle<TextureResource> m_resource = Handle<TextureResource>::null;
	};
}
