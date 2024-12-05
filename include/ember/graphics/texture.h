#pragma once

#include "core/common.h"
#include "core/pool.h"
#include "math/vector2.h"

namespace Ember
{
	enum class TextureFormat
	{
		R8G8B8A8,
		R8,
		Depth24Stencil8,
		Color = R8G8B8A8,
	};

	namespace TextureFormatExt
	{
		u32 size(TextureFormat format);
	}

	class Texture
	{
	public:
		~Texture();

		[[nodiscard]] Vector2u size() const;
		[[nodiscard]] u64 memory_size() const;

	private:
		Vector2u m_size;
		TextureFormat m_format;
		Handle<Texture> m_handle;

		bool m_is_target_attachment = false;
	};
}