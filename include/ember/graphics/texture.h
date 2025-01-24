#pragma once

#include "graphics/render_device.h"
#include "graphics/texture.h"
#include "graphics/texture_format.h"
#include "core/common.h"
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
		glm::uvec2 m_size;
		TextureFormat m_format = TextureFormat::Color;
		Handle<TextureResource> m_resource = Handle<TextureResource>::null;
	};

	inline bool operator==(const Ember::TextureSampler& lhs, const Ember::TextureSampler& rhs) {
		return lhs.filter == rhs.filter && lhs.wrap_x == rhs.wrap_x && lhs.wrap_y == rhs.wrap_y;
	}
}

template<>
struct std::hash<Ember::TextureSampler>
{
	size_t operator()(const Ember::TextureSampler& sampler) const noexcept {
		return combined_hash(sampler.filter, sampler.wrap_x, sampler.wrap_y);
	}
};
