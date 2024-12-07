#pragma once

#include <SDL3/SDL_gpu.h>

#include "texture_format.h"
#include "core/pool.h"
#include "graphics/render_device.h"

namespace Ember
{
	struct ShaderResourceSDL : ShaderResource
	{
		ShaderResourceSDL(SDL_GPUShader* v, SDL_GPUShader* f) : vertex(v), fragment(f) {}

		SDL_GPUShader* vertex;
		SDL_GPUShader* fragment;
	};

	struct TextureResourceSDL : TextureResource
	{
		TextureResourceSDL(SDL_GPUTexture* texture, SDL_GPUTextureFormat format, u32 width, u32 height)
			: texture(texture), format(format), width(width), height(height) {}

		SDL_GPUTexture* texture;
		SDL_GPUTextureFormat format;
		u32 width;
		u32 height;
	};

	class RenderDeviceSDL final : public RenderDevice
	{
	public:
		RenderDeviceSDL() = default;
		~RenderDeviceSDL() override;

		void init(Window *window) override;
		void destroy() override;
		void* native_handle() override { return m_gpu; }

		ShaderHandle create_shader(const ShaderDef &def) override;
		void destroy_shader(ShaderHandle handle) override;

		TextureHandle create_texture(u32 width, u32 height, TextureFormat format) override;
		void destroy_texture(TextureHandle handle) override;

	private:
		bool					m_initialized = false;
		Window*					m_window = nullptr;
		SDL_GPUDevice*			m_gpu = nullptr;

		Pool<ShaderResourceSDL, ShaderResource>		m_shaders;
		Pool<TextureResourceSDL, TextureResource>	m_textures;
	};
}