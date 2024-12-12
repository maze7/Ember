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

	for (auto& m_fence : m_fences) {
		SDL_ReleaseGPUFence(m_gpu, m_fence[0]);
		SDL_ReleaseGPUFence(m_gpu, m_fence[1]);
	}

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

bool RenderDeviceSDL::begin_render_pass(ClearInfo clear, Target* target) {
	if (target == nullptr)
		target = m_framebuffer.get();

	// only begin pass if we're not already in a render pass that is matching
	if (m_render_pass && m_render_pass_target == target)
		return false;

	end_render_pass();

	m_render_pass_target = target;
	std::vector<SDL_GPUTexture*> color_targets;
	color_targets.reserve(4);
	SDL_GPUTexture* depth_stencil_target = nullptr;

	auto target_size = m_render_pass_target->size();
	for (auto& attachment : target->attachments()) {
		auto texture = m_textures.get(attachment.handle());

		if (texture && texture->is_target_attachment) {
			if (texture->format == SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT || texture->format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT)
				depth_stencil_target = texture->texture;
			else
				color_targets.push_back(texture->texture);
		} else {
			throw Exception("Drawing to an invalid texture");
		}
	}

	std::vector<SDL_GPUColorTargetInfo> color_info(color_targets.size());
	SDL_GPUDepthStencilTargetInfo depth_stencil_info;
	auto clear_color = clear.color.value_or(Color::Transparent);

	// get color infos
	for (u8 i = 0; i < color_targets.size(); i++) {
		color_info[i] = SDL_GPUColorTargetInfo{
			.texture = color_targets[i],
			.mip_level = 0,
			.layer_or_depth_plane = 0,
			.clear_color = SDL_FColor{
				.r = (float) clear_color.r / 255,
				.g = (float) clear_color.g / 255,
				.b = (float) clear_color.b / 255,
				.a = (float) clear_color.a / 255,
			},
			.load_op = clear.color.has_value() ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
			.store_op = SDL_GPU_STOREOP_STORE,
			.cycle = clear.color.has_value()
		};
	}

	// get depth info
	if (depth_stencil_target) {
		depth_stencil_info = SDL_GPUDepthStencilTargetInfo{
			.texture = depth_stencil_target,
			.clear_depth = clear.depth.value_or(0),
			.load_op = clear.depth.has_value() ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
			.store_op = SDL_GPU_STOREOP_STORE,
			.stencil_load_op = clear.stencil.has_value() ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
			.stencil_store_op = SDL_GPU_STOREOP_STORE,
			.cycle = clear.depth.has_value() && clear.stencil.has_value(),
			.clear_stencil = (u8)(clear.stencil.value_or(0)),
		};
	}

	// begin render passs
	m_render_pass = SDL_BeginGPURenderPass(
		m_cmd_render,
		color_info.data(),
		color_targets.size(),
		depth_stencil_target ? &depth_stencil_info : nullptr
	);

	return m_render_pass != nullptr;
}

void RenderDeviceSDL::end_render_pass() {
	if (m_render_pass)
		SDL_EndGPURenderPass(m_render_pass);

	m_render_pass = nullptr;
	m_render_pass_target = nullptr;
	// m_render_pass_pipeline = nullptr;
	// m_render_pass_mesh = nullptr;
	// m_render_pass_viewport = nullptr;
	// m_render_pass_scissor = nullptr;
}

void RenderDeviceSDL::clear(Color color, float depth, int stencil, ClearMask mask, Target *target) {
	EMBER_ASSERT(m_initialized);

	if (mask != ClearMask::None) {
		begin_render_pass({
			.color = (i32) mask & (i32) ClearMask::Color ? std::optional<Color>(color) : std::nullopt,
			.depth = (i32) mask & (i32) ClearMask::Depth ? std::optional<float>(depth) : std::nullopt,
			.stencil = (i32) mask & (i32) ClearMask::Stencil ? std::optional<int>(stencil) : std::nullopt,
		}, target);
	}
}

void RenderDeviceSDL::present() {
	end_copy_pass();
	end_render_pass();

	// Wait for the least-recent fence
	if (m_fences[m_frame][0] || m_fences[m_frame][1]) {
		SDL_WaitForGPUFences(m_gpu, true, m_fences[m_frame], 2);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][0]);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][1]);
	}

	// if swapchain can be acquired, blit framebuffer to it
	SDL_GPUTexture* swapchain_texture = nullptr;
	Vector2u swapchain_size;
	if (SDL_AcquireGPUSwapchainTexture(m_cmd_render, m_window->native_handle(), &swapchain_texture, &swapchain_size.x, &swapchain_size.y)) {
		// SDL_AcquireGPUSwapchainTexture can return true, but no texture for a variety of reasons
		// - window is minimized
		// - awaiting previous frame render
		if (swapchain_texture) {
			auto framebuffer = m_textures.get(m_framebuffer->attachments()[0].handle());

			SDL_GPUBlitInfo blit_info;
			blit_info.source.texture = framebuffer->texture;
			blit_info.source.mip_level = 0;
			blit_info.source.layer_or_depth_plane = 0;
			blit_info.source.x = 0;
			blit_info.source.y = 0;
			blit_info.source.w = framebuffer->width;
			blit_info.source.h = framebuffer->height;

			blit_info.destination.texture = swapchain_texture;
			blit_info.destination.mip_level = 0;
			blit_info.destination.layer_or_depth_plane = 0;
			blit_info.destination.x = 0;
			blit_info.destination.y = 0;
			blit_info.destination.w = swapchain_size.x;
			blit_info.destination.h = swapchain_size.y;

			blit_info.load_op = SDL_GPU_LOADOP_DONT_CARE;
			blit_info.clear_color.r = 0;
			blit_info.clear_color.g = 0;
			blit_info.clear_color.b = 0;
			blit_info.clear_color.a = 0;
			blit_info.flip_mode = SDL_FLIP_NONE;
			blit_info.filter = SDL_GPU_FILTER_LINEAR;
			blit_info.cycle = false;

			SDL_BlitGPUTexture(m_cmd_render, &blit_info);

			// resize framebuffer if needed
			if (swapchain_size.x != framebuffer->width || swapchain_size.y != framebuffer->height) {
				m_framebuffer.reset();
				m_framebuffer = std::make_unique<Target>(swapchain_size.x, swapchain_size.y);
				Log::info("Framebuffer recreated: {}x{}", swapchain_size.x, swapchain_size.y);
			}
		}
	}

	// flush commands from this frame
	{
		end_copy_pass();
		end_render_pass();
		m_fences[m_frame][0] = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd_transfer);
		m_fences[m_frame][1] = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd_render);
		m_cmd_transfer = nullptr;
		m_cmd_render = nullptr;
		reset_command_buffers();
	}

	m_frame = (m_frame + 1) % MAX_FRAMES_IN_FLIGHT;
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
	return m_textures.emplace(texture, sdl_format, width, height, target != nullptr);
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
