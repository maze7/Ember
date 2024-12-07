#pragma once

#include <SDL3/SDL_gpu.h>

#include "core/pool.h"
#include "graphics/render_device.h"

namespace Ember
{
	struct ShaderResourceSDL : public ShaderResource
	{
		ShaderResourceSDL(SDL_GPUShader* v, SDL_GPUShader* f) : vertex(v), fragment(f) {}

		SDL_GPUShader* vertex;
		SDL_GPUShader* fragment;
	};

	class RenderDeviceSDL final : public RenderDevice
	{
	public:
		RenderDeviceSDL() = default;
		~RenderDeviceSDL() override;

		void init(Window *window) override;
		void destroy() override;

		ShaderHandle create_shader(const ShaderDef &def) override;
		void destroy_shader(ShaderHandle handle) override;

		operator SDL_GPUDevice* () { return m_gpu; }

	private:
		ShaderResourceSDL get_shader(ShaderHandle handle);
		
		bool					m_initialized = false;
		Window*					m_window = nullptr;
		SDL_GPUDevice*			m_gpu = nullptr;

		Pool<ShaderResourceSDL, ShaderResource>	m_shaders;
	};
}