#pragma once

#include <SDL3/SDL_gpu.h>

#include "graphics/render_device.h"
#include "graphics/texture_format.h"
#include "graphics/target.h"
#include "graphics/color.h"
#include "core/pool.h"

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

		Handle<ShaderResource> create_shader(const ShaderDef &def) override;
		void destroy_shader(Handle<ShaderResource> handle) override;

		Handle<TextureResource> create_texture(u32 width, u32 height, TextureFormat format, Target* target) override;
		void destroy_texture(Handle<TextureResource> handle) override;

		Handle<TargetResource> create_target(u32 width, u32 height) override;
		void destroy_target(Handle<TargetResource> handle) override;

	private:
		struct ClearInfo
		{
			Color color;
			float depth;
			int stencil;
		};

		void reset_command_buffers();
		void flush_commands();
		void begin_copy_pass();
		void end_copy_pass();
		void begin_render_pass(ClearInfo clear, Target* target = nullptr);
		void end_render_pass();

		bool					m_initialized = false;
		Window*					m_window = nullptr;
		SDL_GPUDevice*			m_gpu = nullptr;

		Pool<ShaderResourceSDL, ShaderResource>		m_shaders;
		Pool<TextureResourceSDL, TextureResource>	m_textures;
		Pool<TargetResource>						m_targets;

		Handle<TextureResource>	m_default_texture{};
		std::unique_ptr<Target>	m_framebuffer{};
		SDL_GPUTransferBuffer*  m_texture_transfer_buffer = nullptr;
		SDL_GPUTransferBuffer*  m_buffer_transfer_buffer = nullptr;
		SDL_GPUCommandBuffer*   m_cmd_render = nullptr;
		SDL_GPUCommandBuffer*	m_cmd_transfer = nullptr;
		SDL_GPUCopyPass*		m_copy_pass = nullptr;
		SDL_GPURenderPass*		m_render_pass = nullptr;

		u32 m_texture_transfer_buffer_offset = 0;
		u32 m_texture_transfer_buffer_cycle_count = 0;
		u32 m_buffer_transfer_buffer_offset = 0;
		u32 m_buffer_transfer_buffer_cycle_count = 0;
	};
}
