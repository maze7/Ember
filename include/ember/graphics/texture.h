#pragma once

#include "graphics/render_device.h"
#include "graphics/texture.h"
#include "graphics/texture_format.h"
#include "core/common.h"
#include "math/vector2.h"
#include "core/handle.h"

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

	struct TextureResource;
	class Target;
	class Texture
	{
	public:

		Texture(u32 width, u32 height, TextureFormat format = TextureFormat::Color, Target* target = nullptr);
		~Texture();

		Handle<TextureResource> handle() const { return m_resource; }
		TextureFormat format() const { return m_format; }

	private:
		bool m_is_target_attachment = false;
		Vector2u m_size = Vector2u::zero;
		TextureFormat m_format = TextureFormat::Color;
		Handle<TextureResource> m_resource = Handle<TextureResource>::null;
	};
}
