#include "graphics/render_device_sdl.h"
#include "graphics/shader.h"

using namespace Ember;

void RenderDeviceSDL::init(Window* window) {
	EMBER_ASSERT(!m_initialized);

	m_window = window;
	m_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
	SDL_ClaimWindowForGPUDevice(m_gpu, m_window->native_handle());

	m_initialized = true;
}

void RenderDeviceSDL::destroy() {
	EMBER_ASSERT(m_initialized);
	SDL_WaitForGPUIdle(m_gpu);

	for (auto shader : m_shaders)
		destroy_shader(shader);

	SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window->native_handle());
	SDL_DestroyGPUDevice(m_gpu);
	m_initialized = false;
}

RenderDeviceSDL::~RenderDeviceSDL() {
	if (m_initialized)
		destroy();
}

ShaderHandle RenderDeviceSDL::create_shader(const ShaderDef& def) {
	EMBER_ASSERT(m_initialized);

	SDL_GPUShaderCreateInfo vertex_create_info = {
		.code_size = def.vertex.size(),
		.code = def.vertex.data(),
		.entrypoint = "main",
		.format = SDL_GPU_SHADERFORMAT_SPIRV,
		.stage = SDL_GPU_SHADERSTAGE_VERTEX
	};

	SDL_GPUShaderCreateInfo fragment_create_info = {
		.code_size = def.fragment.size(),
		.code = def.fragment.data(),
		.entrypoint = "main",
		.format = SDL_GPU_SHADERFORMAT_SPIRV,
		.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
	};

	// compile shaders & add to shader pool
	auto vertex = SDL_CreateGPUShader(m_gpu, &vertex_create_info);
	auto fragment = SDL_CreateGPUShader(m_gpu, &fragment_create_info);
	return m_shaders.emplace(vertex, fragment);
}

void RenderDeviceSDL::destroy_shader(ShaderHandle handle) {
	if (auto shader = m_shaders.get(handle)) {
		Log::trace("Destroying shader: [slot: {}, gen: {}]", handle.slot, handle.gen);
		SDL_ReleaseGPUShader(m_gpu, shader->vertex);
		SDL_ReleaseGPUShader(m_gpu, shader->fragment);
		m_shaders.erase(handle);
	}
}
