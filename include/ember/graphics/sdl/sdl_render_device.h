#pragma once

#include <SDL3/SDL_gpu.h>

#include "graphics/render_device.h"

namespace Ember
{
	class SDLRenderDevice final : public RenderDevice
	{
	public:
		SDLRenderDevice();
		~SDLRenderDevice() override;

		void initialize(Window* window);
		void destroy();

		Handle<Shader> create_shader(const ShaderDef& def);
		void destroy_shader(Handle<Shader> shader);

		SDL_GPUDevice* native_handle() const { return m_gpu; }

	private:
		bool m_initialized = false;
		Window* m_window = nullptr;
		SDL_GPUDevice* m_gpu = nullptr;
	};
}