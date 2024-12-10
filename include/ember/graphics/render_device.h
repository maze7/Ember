#pragma once

#include "texture_format.h"
#include "core/common.h"
#include "core/handle.h"
#include "platform/window.h"

namespace Ember
{
	struct ShaderDef;
	struct ShaderResource {};
	using ShaderHandle = Handle<ShaderResource>;

	struct TextureResource {};
	using TextureHandle = Handle<TextureResource>;

	class RenderDevice
	{
	public:
		virtual ~RenderDevice() = default;

		virtual void init(Window* window) = 0;
		virtual void destroy() = 0;
		virtual void* native_handle() = 0;

		// shader resources
		virtual ShaderHandle create_shader(const ShaderDef& def) = 0;
		virtual void destroy_shader(ShaderHandle handle) = 0;

		// texture resources
		virtual TextureHandle create_texture(u32 width, u32 height, TextureFormat format) = 0;
		virtual void destroy_texture(TextureHandle handle) = 0;
	};

	extern RenderDevice* render_device;
}