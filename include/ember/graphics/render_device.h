#pragma once

#include "color.h"
#include "graphics/texture_format.h"
#include "platform/window.h"
#include "core/common.h"
#include "core/handle.h"

namespace Ember
{
	enum class ClearMask : i32
	{
		None = 0,
		Color = 1 << 0,
		Depth = 1 << 1,
		Stencil = 1 << 2,
		All = Color | Depth | Stencil
	};

	struct ShaderDef;
	struct ShaderResource {};
	struct TextureResource {};
	struct TargetResource {};
	struct DrawCommand;
	class Target;

	class RenderDevice
	{
	public:
		virtual ~RenderDevice() = default;

		virtual void init(Window* window) = 0;
		virtual void destroy() = 0;
		virtual void* native_handle() = 0;

		virtual void clear(Color color, float depth, int stencil, ClearMask mask, Target* target = nullptr) = 0;
		virtual void draw(DrawCommand cmd) = 0;
		virtual void present() = 0;

		// shader resources
		virtual Handle<ShaderResource> create_shader(const ShaderDef& def) = 0;
		virtual void destroy_shader(Handle<ShaderResource> handle) = 0;

		// texture resources
		virtual Handle<TextureResource> create_texture(u32 width, u32 height, TextureFormat format, Target* target) = 0;
		virtual void destroy_texture(Handle<TextureResource> handle) = 0;

		// render target resources
		virtual Handle<TargetResource> create_target(u32 width, u32 height) = 0;
		virtual void destroy_target(Handle<TargetResource> handle) = 0;
	};

	extern RenderDevice* render_device;
}