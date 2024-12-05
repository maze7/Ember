#include "graphics/sdl/sdl_render_device.h"

using namespace Ember;

void SDLRenderDevice::initialize(Window *window) {
	EMBER_ASSERT(!m_initialized);

	m_window = window;
	m_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
	SDL_ClaimWindowForGPUDevice(m_gpu, m_window->native_handle());

	m_initialized = true;
}

void SDLRenderDevice::destroy() {
	EMBER_ASSERT(m_initialized);

	SDL_WaitForGPUIdle(m_gpu);
	SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window->native_handle());
	SDL_DestroyGPUDevice(m_gpu);
	m_initialized = false;
}

SDLRenderDevice::SDLRenderDevice() {}

SDLRenderDevice::~SDLRenderDevice() {
	if (m_initialized)
          destroy();
}

Handle<Shader> SDLRenderDevice::create_shader(const ShaderDef& def) {

	SDL_GPUShaderCreateInfo vertex_create_info = {
		.code_size = def.vertex.size(),
		.code = def.vertex.data(),
		.entrypoint = "main",
		.format = SDL_GPU_SHADERFORMAT_SPIRV,
		.stage = SDL_GPU_SHADERSTAGE_VERTEX,
	};

	SDL_GPUShaderCreateInfo fragment_create_info = {
		.code_size = def.fragment.size(),
		.code = def.fragment.data(),
		.entrypoint = "main",
		.format = SDL_GPU_SHADERFORMAT_SPIRV,
		.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
	};

	// build shaders
	SDL_GPUShader* vertex = SDL_CreateGPUShader(m_gpu, &vertex_create_info);
	SDL_GPUShader* fragment = SDL_CreateGPUShader(m_gpu, &fragment_create_info);
	SDL_GPUColorTargetDescription color_targets[] = {{ .format = SDL_GetGPUSwapchainTextureFormat(m_gpu, m_window->native_handle()) }};

	// create the GPU pipeline
	SDL_GPUGraphicsPipelineCreateInfo pso_create_info = {
		.vertex_shader = vertex,
		.fragment_shader = fragment,
		.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
		.rasterizer_state = { .fill_mode = SDL_GPU_FILLMODE_FILL },
		.target_info = {
			.color_target_descriptions = color_targets,
			.num_color_targets = 1,
		},
	};
}

void SDLRenderDevice::destroy_shader(Handle<Shader> shader) {

}