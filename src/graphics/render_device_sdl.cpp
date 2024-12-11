#include "graphics/render_device_sdl.h"
#include "graphics/shader.h"

using namespace Ember;

namespace
{
	SDL_GPUTextureFormat to_sdl_gpu_texture_format(TextureFormat format) {
		switch (format) {
			case TextureFormat::R8G8B8A8:
				return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
			case TextureFormat::R8:
				return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
			case TextureFormat::Depth24Stencil8:
				return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
			default:
				throw Exception("Unknown texture format");
		}
	}
}

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

Handle<ShaderResource> RenderDeviceSDL::create_shader(const ShaderDef& def) {
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

void RenderDeviceSDL::destroy_shader(Handle<ShaderResource> handle) {
	if (auto shader = m_shaders.get(handle)) {
		Log::trace("Destroying shader: [slot: {}, gen: {}]", handle.slot, handle.gen);
		SDL_ReleaseGPUShader(m_gpu, shader->vertex);
		SDL_ReleaseGPUShader(m_gpu, shader->fragment);
		m_shaders.erase(handle);
	}
}

Handle<TextureResource> RenderDeviceSDL::create_texture(u32 width, u32 height, TextureFormat format, Target* target) {
	EMBER_ASSERT(m_initialized);

	auto sdl_format = to_sdl_gpu_texture_format(format);

	SDL_GPUTextureCreateInfo info = {
		.type = SDL_GPU_TEXTURETYPE_2D,
		.format = sdl_format,
		.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER,
		.width = width,
		.height = height,
		.layer_count_or_depth = 1,
		.num_levels = 1,
		.sample_count = SDL_GPU_SAMPLECOUNT_1
	};

	if (target) {
		if (format == TextureFormat::Depth24Stencil8)
			info.usage |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		else
			info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	}

	// create texture resource and add to texture pool
	auto texture = SDL_CreateGPUTexture(m_gpu, &info);
	return m_textures.emplace(texture, sdl_format, width, height);
}

void RenderDeviceSDL::destroy_texture(Handle<TextureResource> handle) {
	if (auto texture = m_textures.get(handle)) {
		Log::trace("Destroying texture: [slot: {}, gen: {}]", handle.slot, handle.gen);
		SDL_ReleaseGPUTexture(m_gpu, texture->texture);
	}
}

Handle<TargetResource> RenderDeviceSDL::create_target(u32 width, u32 height) {
	EMBER_ASSERT(m_initialized);
	return m_targets.emplace();
}

void RenderDeviceSDL::destroy_target(Handle<TargetResource> handle) {
	if (auto target = m_targets.get(handle)) {
		for (auto attachment : target->attachments) {
			destroy_texture(attachment);
		}

		m_targets.erase(handle);
	}
}
