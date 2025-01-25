#include "graphics/render_device_sdl.h"
#include "graphics/material.h"
#include "core/hash.h"
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

	SDL_GPUIndexElementSize to_sdl_index_format(IndexFormat format) {
		switch (format) {
			case IndexFormat::Sixteen:
				return SDL_GPU_INDEXELEMENTSIZE_16BIT;
			case IndexFormat::ThirtyTwo:
				return SDL_GPU_INDEXELEMENTSIZE_32BIT;
			default:
				throw Exception("Unknown index format");
		}
	};

	SDL_GPUVertexElementFormat to_sdl_vertex_format(VertexType type, bool normalized) {
		switch (type) {
			case VertexType::Float:
				return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
			case VertexType::Float2:
				return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
			case VertexType::Float3:
				return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
			case VertexType::Float4:
				return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
			case VertexType::Byte4:
				return normalized ? SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM : SDL_GPU_VERTEXELEMENTFORMAT_BYTE4;
			case VertexType::UByte4:
				return normalized ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM : SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
			case VertexType::Short2:
				return normalized ? SDL_GPU_VERTEXELEMENTFORMAT_BYTE2_NORM : SDL_GPU_VERTEXELEMENTFORMAT_BYTE2;
			case VertexType::UShort2:
				return normalized ? SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2_NORM : SDL_GPU_VERTEXELEMENTFORMAT_UBYTE2;
			case VertexType::Short4:
				return normalized ? SDL_GPU_VERTEXELEMENTFORMAT_SHORT4_NORM : SDL_GPU_VERTEXELEMENTFORMAT_SHORT4;
			case VertexType::UShort4:
				return normalized ? SDL_GPU_VERTEXELEMENTFORMAT_USHORT4_NORM : SDL_GPU_VERTEXELEMENTFORMAT_USHORT4;
			default:
				throw Exception("Unknown vertex type");
		}
	};

	SDL_GPUSamplerAddressMode to_sdl_wrap_mode(TextureWrap wrap) {
		switch (wrap) {
			case TextureWrap::Repeat:
				return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
			case TextureWrap::MirroredRepeat:
				return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
			case TextureWrap::Clamp:
				return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			default:
				throw Exception("Unkown texture wrap mode");
		}
	}

}

void RenderDeviceSDL::init(Window* window) {
	EMBER_ASSERT(!m_initialized);

	m_window = window;
	m_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
	SDL_ClaimWindowForGPUDevice(m_gpu, m_window->native_handle());

	// set present mode depending on what is available
	if (SDL_WindowSupportsGPUPresentMode(m_gpu, m_window->native_handle(), SDL_GPU_PRESENTMODE_IMMEDIATE)) {
		SDL_SetGPUSwapchainParameters(m_gpu, m_window->native_handle(), SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE);
	}

	// init command buffers
	reset_command_buffers();

	// create transfer buffers (textures & buffer)
	SDL_GPUTransferBufferCreateInfo info = {
		.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
		.size = 16 * 1024 * 1024, // 16mb
		.props = 0,
	};
	m_texture_upload_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &info);
	m_buffer_upload_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &info);
	m_initialized = true;

	// create default texture
	m_default_texture = create_texture(1, 1, TextureFormat::R8G8B8A8, nullptr);
	// upload default white texture data

	// create framebuffer
	auto window_size = window->size();
	m_framebuffer = make_ref<Target>((u32)window_size.x, (u32)window_size.y);

}

void RenderDeviceSDL::dispose() {
	EMBER_ASSERT(m_initialized);
	SDL_WaitForGPUIdle(m_gpu);

	m_framebuffer.reset();

	// destroy cached samplers
	for (auto& [key, sampler] : m_sampler_cache)
		SDL_ReleaseGPUSampler(m_gpu, sampler);

	// dispose of allocated GPU render targets
	for (auto target : m_targets)
		dispose_target(target.handle());

	// dispose of allocated GPU shaders
	for (auto shader : m_shaders)
		dispose_shader(shader.handle());

	// dispose of allocated GPU textures
	for (auto texture : m_textures)
		dispose_texture(texture.handle());

	// dispose of allocated GPU fences
	for (auto& m_fence : m_fences) {
		if (m_fence[0])
			SDL_ReleaseGPUFence(m_gpu, m_fence[0]);

		if (m_fence[1])
			SDL_ReleaseGPUFence(m_gpu, m_fence[1]);
	}

	// destroy transfer buffers
	SDL_ReleaseGPUTransferBuffer(m_gpu, m_texture_upload_buffer);
	SDL_ReleaseGPUTransferBuffer(m_gpu, m_buffer_upload_buffer);

	SDL_ReleaseWindowFromGPUDevice(m_gpu, m_window->native_handle());
	SDL_DestroyGPUDevice(m_gpu);

	m_initialized = false;
}

RenderDeviceSDL::RenderDeviceSDL(): m_pso_shaders() {}

RenderDeviceSDL::~RenderDeviceSDL() {
	if (m_initialized)
		dispose();
}

void RenderDeviceSDL::reset_command_buffers() {
	EMBER_ASSERT(m_cmd_render == nullptr && m_cmd_transfer == nullptr);

	m_cmd_render = SDL_AcquireGPUCommandBuffer(m_gpu);
	m_cmd_transfer = SDL_AcquireGPUCommandBuffer(m_gpu);

	m_texture_upload_buffer_offset = 0;
	m_texture_upload_buffer_cycle_count = 0;
	m_buffer_upload_buffer_offset = 0;
	m_buffer_upload_buffer_offset = 0;
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
		return true;

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
	m_render_pass_pso = nullptr;
	m_render_pass_mesh = nullptr;
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

SDL_GPUSampler* RenderDeviceSDL::get_sampler(TextureSampler sampler) {
	// check if the sampler already exists in the cache
	if (auto it = m_sampler_cache.find(sampler); it != m_sampler_cache.end())
		return it->second;

	// convert TextureSampler properties to SDL_GPU types
	SDL_GPUFilter filter = (sampler.filter == TextureFilter::Nearest)
							? SDL_GPU_FILTER_NEAREST
							: (sampler.filter == TextureFilter::Linear)
								? SDL_GPU_FILTER_LINEAR
								: throw Exception("Unkown texture filter");

	SDL_GPUSamplerCreateInfo info = {
		.min_filter = filter,
		.mag_filter = filter,
		.address_mode_u = to_sdl_wrap_mode(sampler.wrap_x),
		.address_mode_v = to_sdl_wrap_mode(sampler.wrap_y),
		.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT,
		.compare_op = SDL_GPU_COMPAREOP_ALWAYS,
		.enable_compare = false,
	};

	// create the sampler
	SDL_GPUSampler* result = SDL_CreateGPUSampler(m_gpu, &info);
	if (!result)
		throw Exception("Failed to create GPU sampler: " + std::string(SDL_GetError()));

	// cache the newly created sampler & return it
	m_sampler_cache[sampler] = result;
	return result;
}

void RenderDeviceSDL::draw(DrawCommand cmd) {
	EMBER_ASSERT(m_initialized);

	auto& mat = cmd.material;
	auto& shader = mat->shader();
	auto& target = cmd.target;
	auto& mesh = cmd.mesh;

	if (!begin_render_pass(ClearInfo(), target.get()))
		return;

	// set scissor
	{
		SDL_Rect rect{ 0, 0, static_cast<int>(m_render_pass_target->size().x), static_cast<int>(m_render_pass_target->size().y) };
		SDL_SetGPUScissor(m_render_pass, &rect);
	}

	// set viewport
	{
		SDL_GPUViewport viewport{
			.x = 0, .y = 0,
			.w = static_cast<float>(m_render_pass_target->size().x),
			.h = static_cast<float>(m_render_pass_target->size().y),
			.min_depth = 0, .max_depth = 1,
		};
		SDL_SetGPUViewport(m_render_pass, &viewport);
	}

	// figure out graphics pipeline, potentially create a new one on-demand
	auto pso = get_pso(cmd);
	if (pso != m_render_pass_pso) {
		SDL_BindGPUGraphicsPipeline(m_render_pass, pso);
		m_render_pass_pso = pso;
	}

	// bind mesh buffers
	auto mesh_resource = m_meshes.get(mesh->resource());
	EMBER_ASSERT(mesh_resource != nullptr);

	if (m_render_pass_mesh != mesh_resource || mesh_resource->vertex.dirty || mesh_resource->index.dirty || mesh_resource->instance.dirty) {
		m_render_pass_mesh = mesh_resource;
		mesh_resource->vertex.dirty = false;
		mesh_resource->index.dirty = false;
		mesh_resource->instance.dirty = false;

		// bind index buffer
		SDL_GPUBufferBinding index_binding = {
			.buffer = mesh_resource->index.handle,
			.offset = 0
		};
		SDL_BindGPUIndexBuffer(m_render_pass, &index_binding, to_sdl_index_format(mesh_resource->index_format));

		// bind vertex buffer
		SDL_GPUBufferBinding vertex_binding = {
			.buffer = mesh_resource->vertex.handle,
			.offset = 0
		};
		SDL_BindGPUVertexBuffers(m_render_pass, 0, &vertex_binding, 1);
	}

	// bind fragment samplers
	if (shader.fragment().num_samplers > 0) {
		auto default_texture_resource = m_textures.get(m_default_texture);

		std::vector<SDL_GPUTextureSamplerBinding> samplers(shader.fragment().num_samplers);
		for (u32 i = 0; i < shader.fragment().num_samplers; i++) {
			if (auto& tex = mat->fragment_samplers()[i].texture; tex != nullptr) {
				auto tex_resource = m_textures.get(tex->handle());
				if (tex_resource) {
					samplers[i].texture = tex_resource->texture;
				} else {
					samplers[i].texture = default_texture_resource->texture;
				}
			} else {
				samplers[i].texture = default_texture_resource->texture;
			}

			samplers[i].sampler = get_sampler(mat->fragment_samplers()[i].sampler);
		}

		SDL_BindGPUFragmentSamplers(m_render_pass, 0, samplers.data(), (u32)shader.fragment().num_samplers);
	}

	// upload vertex uniforms
	if (!shader.vertex().uniforms.empty())
		SDL_PushGPUVertexUniformData(m_cmd_render, 0, (const void*)mat->vertex_data(), shader.vertex().uniform_buffer_size());

	// upload fragment uniforms
	if (!shader.fragment().uniforms.empty())
		SDL_PushGPUFragmentUniformData(m_cmd_render, 0, (const void*)mat->fragment_data(), shader.fragment().uniform_buffer_size());

	// perform draw
	SDL_DrawGPUIndexedPrimitives(m_render_pass, cmd.mesh_index_count, 1, cmd.mesh_index_start, cmd.mesh_vertex_offset, 0);
}

SDL_GPUGraphicsPipeline* RenderDeviceSDL::get_pso(DrawCommand cmd) {
	EMBER_ASSERT(m_initialized);

	auto hash = combined_hash(
		cmd.target,
		cmd.material->shader().handle(),
		cmd.mesh->resource()
	);

	if (!cmd.target)
		cmd.target = m_framebuffer;

	auto& shader = cmd.material->shader();
	auto shader_res = m_shaders.get(shader.handle());
	auto target = cmd.target;
	auto mesh = cmd.mesh;
	auto vertex_format = mesh->vertex_format();
	SDL_GPUGraphicsPipeline* pipeline = nullptr;

	if (!m_pso_cache.contains(hash)) {
		SDL_GPUTextureFormat depth_stencil_attachment = SDL_GPU_TEXTUREFORMAT_INVALID;
		SDL_GPUColorTargetDescription color_attachments[4];
		SDL_GPUColorTargetBlendState color_blend_state{};
		u8 color_attachment_count = 0;
		SDL_GPUVertexBufferDescription vertex_bindings[1];
		SDL_GPUVertexAttribute vertex_attributes[vertex_format.elements.size()];
		u32 vertex_offset = 0;

		if (cmd.target) {
			for (auto& t : cmd.target->attachments()) {
				if (t.format() == TextureFormat::Depth24Stencil8) {
					depth_stencil_attachment = to_sdl_gpu_texture_format(t.format());
				} else {
					color_attachments[color_attachment_count++] = {
						.format = to_sdl_gpu_texture_format(t.format()),
						.blend_state = color_blend_state,
					};
				}
			}
		}

		vertex_bindings[0] = {
			.slot = 0,
			.pitch = (u32) vertex_format.stride,
			.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
			.instance_step_rate = 0,
		};

		for (int i = 0; i < vertex_format.elements.size(); i++) {
			auto& el = vertex_format.elements[i];
			vertex_attributes[i] = {
				.location = (u32) el.index,
				.buffer_slot = 0,
				.format = to_sdl_vertex_format(el.type, el.normalized),
				.offset = (u32) vertex_offset
			};
			vertex_offset += VertexTypeExt::size(el.type);
		}

		SDL_GPUGraphicsPipelineCreateInfo info = {
			.vertex_shader = shader_res->vertex,
			.fragment_shader = shader_res->fragment,
			.vertex_input_state = {
				.vertex_buffer_descriptions = vertex_bindings,
				.num_vertex_buffers = 1,
				.vertex_attributes = vertex_attributes,
				.num_vertex_attributes = (u32) vertex_format.elements.size(),
			},
			.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
			.rasterizer_state = {
				.fill_mode = SDL_GPU_FILLMODE_FILL,
				.cull_mode = SDL_GPU_CULLMODE_NONE,
				.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
				.enable_depth_bias = false,
			},
			.multisample_state = {
				.sample_count = SDL_GPU_SAMPLECOUNT_1,
				.sample_mask = 0xFFFFFFFF
			},
			.depth_stencil_state = {
				.compare_op = SDL_GPU_COMPAREOP_NEVER,
				.compare_mask = 0xFF,
				.write_mask = 0xFF,
				.enable_depth_test = false,
				.enable_depth_write = false,
				.enable_stencil_test = false,
			},
			.target_info = {
				.color_target_descriptions = color_attachments,
				.num_color_targets = color_attachment_count,
				.depth_stencil_format = depth_stencil_attachment,
				.has_depth_stencil_target = depth_stencil_attachment != SDL_GPU_TEXTUREFORMAT_INVALID,
			}
		};

		pipeline = SDL_CreateGPUGraphicsPipeline(m_gpu, &info);
		if (!pipeline)
			throw Exception("Unable to create PSO");

		// track which shader this pipeline uses
		if (!m_pso_shaders.contains(shader.handle()))
			m_pso_shaders[shader.handle()] = std::vector<SDL_GPUGraphicsPipeline*>();
		m_pso_shaders[shader.handle()].push_back(pipeline);

		// store the PSO in the pso cache
		m_pso_cache[hash] = pipeline;
	}

	return m_pso_cache[hash];
}

void RenderDeviceSDL::present() {
	end_copy_pass();
	end_render_pass();

	// wait for fences for the current frame to complete
	if (m_fences[m_frame][0] || m_fences[m_frame][1]) {
		SDL_WaitForGPUFences(m_gpu, true, m_fences[m_frame], 2);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][0]);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][1]);
	}

	// if swapchain can be acquired, blit framebuffer to it
	SDL_GPUTexture* swapchain_texture = nullptr;
	glm::uvec2 swapchain_size;
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

	flush_commands_and_acquire_fences();
	m_frame = (m_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void RenderDeviceSDL::flush_commands_and_acquire_fences() {
	end_copy_pass();
	end_render_pass();

	m_fences[m_frame][0] = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd_transfer);
	m_fences[m_frame][1] = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd_render);

	if (!m_fences[m_frame][0]) {
		Log::warn("Unable to acquire upload fence: {}", SDL_GetError());
	} else if (!m_fences[m_frame][1]) {
		Log::warn("Unable to acquire render fence: {}", SDL_GetError());
	}

	m_cmd_transfer = nullptr;
	m_cmd_render = nullptr;
	reset_command_buffers();
}

void RenderDeviceSDL::flush_commands_and_stall() {
	flush_commands_and_acquire_fences();

	if (m_fences[m_frame][0] || m_fences[m_frame][1]) {
		SDL_WaitForGPUFences(m_gpu, true, m_fences[m_frame], 2);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][0]);
		SDL_ReleaseGPUFence(m_gpu, m_fences[m_frame][1]);
	}
}

Handle<ShaderResource> RenderDeviceSDL::create_shader(const ShaderDef& def) {
	EMBER_ASSERT(m_initialized);

	SDL_GPUShaderCreateInfo vertex_create_info = {
		.code_size = def.vertex.code.size(),
		.code = def.vertex.code.data(),
		.entrypoint = "main",
		.format = SDL_GPU_SHADERFORMAT_SPIRV,
		.stage = SDL_GPU_SHADERSTAGE_VERTEX,
		.num_samplers = def.vertex.num_samplers,
		.num_uniform_buffers = (u32)(def.vertex.uniforms.empty() ? 0 : 1),
	};

	SDL_GPUShaderCreateInfo fragment_create_info = {
		.code_size = def.fragment.code.size(),
		.code = def.fragment.code.data(),
		.entrypoint = "main",
		.format = SDL_GPU_SHADERFORMAT_SPIRV,
		.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
		.num_samplers = def.fragment.num_samplers,
		.num_uniform_buffers = (u32)(def.fragment.uniforms.empty() ? 0 : 1),
	};

	// compile shaders & add to shader pool
	auto vertex = SDL_CreateGPUShader(m_gpu, &vertex_create_info);
	auto fragment = SDL_CreateGPUShader(m_gpu, &fragment_create_info);
	return m_shaders.emplace(vertex, fragment);
}

void RenderDeviceSDL::dispose_shader(Handle<ShaderResource> handle) {
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

void RenderDeviceSDL::dispose_texture(Handle<TextureResource> handle) {
	if (auto texture = m_textures.get(handle)) {
		Log::trace("Destroying texture: [slot: {}, gen: {}]", handle.slot, handle.gen);
		SDL_ReleaseGPUTexture(m_gpu, texture->texture);
	}
}

void RenderDeviceSDL::set_texture_data(Handle<TextureResource> handle, std::span<std::byte> data) {
	EMBER_ASSERT(m_initialized);

	static constexpr auto round_alignment = [](u32 value, u32 alignment) {
		return alignment * ((value + alignment - 1) / alignment);
	};

	// get the texture
	auto tex = m_textures.get(handle);
	if (tex == nullptr)
		throw Exception("Tried to upload pixel data to a disposed texture");

	bool cycle = m_texture_upload_buffer_offset == 0;
	bool use_temp_upload_buffer = false;
	SDL_GPUTransferBuffer* upload_buffer = m_texture_upload_buffer;
	u32 upload_offset = 0;

	m_texture_upload_buffer_offset = round_alignment(
		m_texture_upload_buffer_offset,
		SDL_GPUTextureFormatTexelBlockSize(tex->format)
	);
	upload_offset = m_texture_upload_buffer_offset;

	// acquire transfer buffer
	if (data.size() >= UPLOAD_BUFFER_SIZE) {
		SDL_GPUTransferBufferCreateInfo info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = static_cast<u32>(data.size()),
			.props = 0
		};
		upload_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &info);
		use_temp_upload_buffer = true;
		cycle = false;
		upload_offset = 0;
	} else if (m_texture_upload_buffer_offset + data.size() >= UPLOAD_BUFFER_SIZE) {
		if (m_texture_upload_buffer_cycle_count < MAX_UPLOAD_CYCLE_COUNT) {
			cycle = true;
			m_texture_upload_buffer_cycle_count++;
			m_texture_upload_buffer_offset = 0;
			upload_offset = 0;
		} else {
			flush_commands_and_stall();
			begin_copy_pass();
			cycle = true;
			upload_offset = 0;
		}
	}

	// copy data
	std::byte* dst = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(m_gpu, upload_buffer, cycle)) + upload_offset;
	std::memcpy(dst, data.data(), data.size());
	SDL_UnmapGPUTransferBuffer(m_gpu, upload_buffer);

	// upload to the GPU
	begin_copy_pass();
	SDL_GPUTextureTransferInfo info = {
		.transfer_buffer = upload_buffer,
		.offset = upload_offset,
		.pixels_per_row = tex->width,  // TODO: FNA3D uses 0
		.rows_per_layer = tex->height, // TODO: FNA3D uses 0
	};

	SDL_GPUTextureRegion region = {
		.texture = tex->texture,
		.mip_level = 0,
		.layer = 0,
		.x = 0,
		.y = 0,
		.z = 0,
		.w = tex->width,
		.h = tex->height,
		.d = 1
	};
	SDL_UploadToGPUTexture(m_copy_pass, &info, &region, cycle); // TODO: Foster uses false, should I too?

	// upload buffer management
	if (use_temp_upload_buffer)
		SDL_ReleaseGPUTransferBuffer(m_gpu, upload_buffer);
	else
		m_texture_upload_buffer_offset += data.size();
}

Handle<TargetResource> RenderDeviceSDL::create_target(u32 width, u32 height) {
	EMBER_ASSERT(m_initialized);
	return m_targets.emplace();
}

void RenderDeviceSDL::dispose_target(Handle<TargetResource> handle) {
	if (auto target = m_targets.get(handle)) {
		m_targets.erase(handle);
	}
}

Handle<MeshResource> RenderDeviceSDL::create_mesh(VertexFormat vertex_format, IndexFormat index_format) {
	EMBER_ASSERT(m_initialized);
	return m_meshes.emplace(vertex_format, index_format);
}

void RenderDeviceSDL::dispose_mesh(Handle<MeshResource> handle) {
	if (auto mesh = m_meshes.get(handle)) {
		Log::trace("Destroying mesh: [slot: {}, gen: {}]", handle.slot, handle.gen);

		static auto destroy_mesh_buffer = [&](MeshResourceSDL::Buffer& buffer) {
			if (buffer.handle != nullptr) {
				SDL_ReleaseGPUBuffer(m_gpu, buffer.handle);
			}
			buffer.handle = nullptr;
		};

		destroy_mesh_buffer(mesh->index);
		destroy_mesh_buffer(mesh->vertex);
		destroy_mesh_buffer(mesh->instance);
		m_meshes.erase(handle);
	}
}

void RenderDeviceSDL::set_mesh_vertex_data(Handle<MeshResource> handle, const void *data, int data_size, int data_dst_offset) {
	EMBER_ASSERT(m_initialized);

	if (auto mesh = m_meshes.get(handle)) {
		mesh->vertex.dirty = true;
		upload_mesh_buffer(mesh->vertex, data, data_size, data_dst_offset, SDL_GPU_BUFFERUSAGE_VERTEX);
	} else {
		Log::error("Tried set vertex data on invalid mesh!");
	}
}

void RenderDeviceSDL::set_mesh_index_data(Handle<MeshResource> handle, const void *data, int data_size, int data_dst_offset) {
	EMBER_ASSERT(m_initialized);

	if (auto mesh = m_meshes.get(handle)) {
		mesh->index.dirty = true;
		upload_mesh_buffer(mesh->index, data, data_size, data_dst_offset, SDL_GPU_BUFFERUSAGE_INDEX);
	} else {
		Log::error("Tried to set index data on invalid mesh!");
	}
}

void RenderDeviceSDL::upload_mesh_buffer(MeshResourceSDL::Buffer& buf, const void *data, int data_size, int data_dst_offset, SDL_GPUBufferUsageFlags usage) {
	// (re)create buffer if needed
	u32 required = data_size + data_dst_offset;
	if (required > buf.capacity || buf.handle == nullptr) {
		// TODO: A resize wipes all contents, not particularly ideal
		if (buf.handle != nullptr) {
			SDL_ReleaseGPUBuffer(m_gpu, buf.handle);
			buf.handle = nullptr;
		}

		// TODO: Upon first creation we should probably just create a perfectly sized buffer, and afterward resize to next Po2
		u32 size;
		if (buf.capacity == 0) {
			size = Math::max((u32) 8, required); // never create a buffer that has 0 length
		} else {
			size = 8;
			while (size < required)
				size *= 2;
		}

		SDL_GPUBufferCreateInfo info = {
			.usage = usage,
			.size = size,
			.props = 0,
		};

		buf.handle = SDL_CreateGPUBuffer(m_gpu, &info);

		if (buf.handle == nullptr) {
			throw Exception("Mesh Creation Failed");
		}
		buf.capacity = size;
	}

	// early exit if there's no data to upload at this time
	if (data == nullptr)
		return;

	bool cycle = true; // TODO: This is controlled by hints/logic in FNA3D, where it can lead to a potential flush
	bool upload_cycle = m_buffer_upload_buffer_offset == 0;
	bool use_temp_upload_buffer = false;
	SDL_GPUTransferBuffer* upload_buffer = m_buffer_upload_buffer;
	u32 upload_offset = m_buffer_upload_buffer_offset;

	// acquire transfer buffer
	if (data_size >= UPLOAD_BUFFER_SIZE) {
		SDL_GPUTransferBufferCreateInfo info = {
			.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			.size = (u32) data_size,
			.props = 0,
		};
		upload_buffer = SDL_CreateGPUTransferBuffer(m_gpu, &info);
		use_temp_upload_buffer = true;
		upload_offset = 0;
	} else if (m_buffer_upload_buffer_offset + data_size >= UPLOAD_BUFFER_SIZE) {
		if (m_buffer_upload_buffer_cycle_count < MAX_UPLOAD_CYCLE_COUNT) {
			upload_cycle = true;
			m_buffer_upload_buffer_cycle_count++;
			m_buffer_upload_buffer_offset = 0;
			upload_offset = 0;
		}
	}

	// copy data
	{
		std::byte* dst = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(m_gpu, upload_buffer, upload_cycle)) + upload_offset;
		memcpy(dst, data, data_size);
		SDL_UnmapGPUTransferBuffer(m_gpu, upload_buffer);
	}

	// submit to the GPU
	{
		begin_copy_pass();

		SDL_GPUTransferBufferLocation location = {
			.transfer_buffer = upload_buffer,
			.offset = upload_offset,
		};

		SDL_GPUBufferRegion region = {
			.buffer = buf.handle,
			.offset = (u32) data_dst_offset,
			.size = (u32) data_size,
		};

		SDL_UploadToGPUBuffer(m_copy_pass, &location, &region, cycle);
	}

	// transfer buffer management
	if (use_temp_upload_buffer) {
		SDL_ReleaseGPUTransferBuffer(m_gpu, upload_buffer);
	} else {
		m_buffer_upload_buffer_offset += (u32) data_size;
	}
}

