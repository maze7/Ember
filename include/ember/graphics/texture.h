#pragma once

#include <span>

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

		Texture(u32 width, u32 height, std::span<std::byte> pixels);

		~Texture();

		[[nodiscard]] Handle<TextureResource> handle() const { return m_handle; }
		[[nodiscard]] TextureFormat format() const { return m_format; }
		[[nodiscard]] u32 memory_size() const { return m_size.x * m_size.y * TextureFormatExt::size(m_format); }

		template <class T>
		void set_data(std::span<T> data) {
			if (sizeof(T) * data.size() < memory_size())
				throw Exception("Data buffer is smaller than the size of the Texture");

			render_device->set_texture_data(m_handle, data);
		}

		auto size() const { return m_size; }

		static Ref<Texture> load(std::string_view path);

	private:
		bool m_is_target_attachment = false;
		glm::uvec2 m_size;
		TextureFormat m_format = TextureFormat::Color;
		Handle<TextureResource> m_handle = Handle<TextureResource>::null;
		u32 m_memory_size = 0;
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
