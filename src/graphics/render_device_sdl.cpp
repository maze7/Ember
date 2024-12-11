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
			case TextureFormat::Color:
				return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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

	// init command buffers
	reset_command_buffers();

	// create transfer buffers (textures & buffer)
	SDL_GPUTransferBufferCreateInfo info = {
		.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
		.size = 16 * 1024 * 1024, // 16mb
		.props = 0,
	};
	m_texture_transfer_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &info);
	m_buffer_transfer_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &info);
	m_initialized = true;

	// create default texture
	m_default_texture = create_texture(1, 1, TextureFormat::R8G8B8A8, nullptr);
	// upload default white texture data

	// create framebuffer
	auto window_size = window->size();
	m_framebuffer = std::make_unique<Target>((u32)window_size.x, (u32)window_size.y);

}

void RenderDeviceSDL::destroy() {
	EMBER_ASSERT(m_initialized);
	SDL_WaitForGPUIdle(m_gpu);

	m_framebuffer.reset();

	for (auto target : m_targets)
		destroy_target(target);

	for (auto shader : m_shaders)
		destroy_shader(shader);

	for (auto texture : m_textures)
		destroy_texture(texture);

	// destroy transfer buffers
	SDL_ReleaseGPUTransferBuffer(m_gpu, m_texture_transfer_buffer);
	SDL_ReleaseGPUTransferBuffer(m_gpu, m_buffer_transfer_buffer);

	SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window->native_handle());
	SDL_DestroyGPUDevice(m_gpu);

	m_initialized = false;
}

RenderDeviceSDL::~RenderDeviceSDL() {
	if (m_initialized)
		destroy();
}

void RenderDeviceSDL::reset_command_buffers() {
	EMBER_ASSERT(m_cmd_render == nullptr && m_cmd_transfer == nullptr);

	m_cmd_render = SDL_AcquireGPUCommandBuffer(m_gpu);
	m_cmd_transfer = SDL_AcquireGPUCommandBuffer(m_gpu);

	m_texture_transfer_buffer_offset = 0;
	m_texture_transfer_buffer_cycle_count = 0;
	m_buffer_transfer_buffer_offset = 0;
	m_buffer_transfer_buffer_offset = 0;
}

void RenderDeviceSDL::flush_commands() {
	end_copy_pass();
	end_render_pass();
	SDL_SubmitGPUCommandBuffer(m_cmd_transfer);
	SDL_SubmitGPUCommandBuffer(m_cmd_render);
	m_cmd_render = nullptr;
	m_cmd_transfer = nullptr;
	reset_command_buffers();
}

void RenderDeviceSDL::begin_copy_pass() {
	if (m_copy_pass)
		return;

	m_copy_pass = SDL_BeginGPUCopyPass(m_cmd_transfer);
}

void RenderDeviceSDL::end_copy_pass() {
	if (m_copy_pass != nullptr)
		SDL_EndGPUCopyPass(m_copy_pass);
	m_copy_pass = nullptr;
}

void RenderDeviceSDL::begin_render_pass(ClearInfo clear, Target* target) {

}

void RenderDeviceSDL::end_render_pass() {

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
		m_targets.erase(handle);
	}
}
