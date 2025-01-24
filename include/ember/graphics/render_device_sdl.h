#pragma once

#include <optional>
#include <SDL3/SDL_gpu.h>

#include "vertex_format.h"
#include "graphics/render_device.h"
#include "graphics/texture_format.h"
#include "graphics/target.h"
#include "graphics/draw_command.h"
#include "graphics/color.h"
#include "core/pool.h"

namespace Ember
{
	enum class IndexFormat;

	struct ShaderResourceSDL : ShaderResource
	{
		ShaderResourceSDL(SDL_GPUShader* v, SDL_GPUShader* f) : vertex(v), fragment(f) {}

		SDL_GPUShader* vertex;
		SDL_GPUShader* fragment;
	};

	struct TextureResourceSDL : TextureResource
	{
		TextureResourceSDL(SDL_GPUTexture* texture, SDL_GPUTextureFormat format, u32 width, u32 height, bool is_target = false)
			: texture(texture), format(format), width(width), height(height), is_target_attachment(is_target) {}

		SDL_GPUTexture* texture;
		SDL_GPUTextureFormat format;
		u32 width;
		u32 height;
		bool is_target_attachment;
	};

	struct MeshResourceSDL : MeshResource
	{
		struct Buffer
		{
			SDL_GPUBuffer* handle = nullptr;
			u32 capacity = 0;
			bool dirty = false;
		};

		MeshResourceSDL(VertexFormat& v_format, IndexFormat i_format)
			: vertex_format(std::move(v_format)), index_format(i_format) {}

		Buffer index;
		Buffer vertex;
		Buffer instance;
		VertexFormat vertex_format;
		IndexFormat index_format;
	};

	class RenderDeviceSDL final : public RenderDevice
	{
	public:
		static constexpr u32 MAX_FRAMES_IN_FLIGHT = 3;

		RenderDeviceSDL();
		~RenderDeviceSDL() override;

		void init(Window *window) override;
		void dispose() override;
		void* native_handle() override { return m_gpu; }

		void clear(Color color, float depth, int stencil, ClearMask mask, Target* target = nullptr) override;
		void draw(DrawCommand cmd) override;
		void present() override;

		Handle<ShaderResource> create_shader(const ShaderDef &def) override;
		void dispose_shader(Handle<ShaderResource> handle) override;

		Handle<TextureResource> create_texture(u32 width, u32 height, TextureFormat format, Target* target) override;
		void dispose_texture(Handle<TextureResource> handle) override;

		Handle<TargetResource> create_target(u32 width, u32 height) override;
		void dispose_target(Handle<TargetResource> handle) override;

		Handle<MeshResource> create_mesh(VertexFormat vertex_format, IndexFormat index_format) override;
		void dispose_mesh(Handle<MeshResource> handle) override;
		void set_mesh_vertex_data(Handle<MeshResource> handle, const void* data, int data_size, int data_dst_offset) override;
		void set_mesh_index_data(Handle<MeshResource> handle, const void* data, int data_size, int data_dst_offset) override;

	private:
		static constexpr u32 UPLOAD_BUFFER_SIZE = 16 * 1024 * 1024; // 16mb
		static constexpr u32 MAX_UPLOAD_CYCLE_COUNT = 4;

		struct ClearInfo
		{
			std::optional<Color> color;
			std::optional<float> depth;
			std::optional<int> stencil;
		};

		void reset_command_buffers();
		void flush_commands();
		void flush_commands_and_acquire_fences();
		void begin_copy_pass();
		void end_copy_pass();
		bool begin_render_pass(ClearInfo clear, Target* target = nullptr);
		void end_render_pass();
		void upload_mesh_buffer(MeshResourceSDL::Buffer& buf, const void* data, int data_size, int data_dst_offset, SDL_GPUBufferUsageFlags usage);
		SDL_GPUGraphicsPipeline* get_pso(DrawCommand cmd);
		SDL_GPUSampler* get_sampler(TextureSampler sampler);

		u32 m_frame = 0;
		SDL_GPUFence* m_fences[MAX_FRAMES_IN_FLIGHT][2] = {
			{ nullptr, nullptr },
			{ nullptr, nullptr },
		};

		bool					m_initialized = false;
		Window*					m_window = nullptr;
		SDL_GPUDevice*			m_gpu = nullptr;

		Pool<ShaderResourceSDL, ShaderResource>		m_shaders;
		Pool<TextureResourceSDL, TextureResource>	m_textures;
		Pool<TargetResource>						m_targets;
		Pool<MeshResourceSDL, MeshResource>			m_meshes;

		Handle<TextureResource>	m_default_texture{};
		std::unique_ptr<Target>	m_framebuffer{};
		SDL_GPUTransferBuffer*  m_texture_upload_buffer = nullptr;
		SDL_GPUTransferBuffer*  m_buffer_upload_buffer = nullptr;
		SDL_GPUCommandBuffer*   m_cmd_render = nullptr;
		SDL_GPUCommandBuffer*	m_cmd_transfer = nullptr;
		SDL_GPUCopyPass*		m_copy_pass = nullptr;
		SDL_GPURenderPass*		m_render_pass = nullptr;
		Target*					m_render_pass_target = nullptr;
		MeshResourceSDL*		m_render_pass_mesh = nullptr;

		std::unordered_map<u64, SDL_GPUGraphicsPipeline*> m_pso_cache;
		std::unordered_map<Handle<ShaderResource>, std::vector<SDL_GPUGraphicsPipeline*>> m_pso_shaders;
		std::unordered_map<TextureSampler, SDL_GPUSampler*> m_sampler_cache;
		SDL_GPUGraphicsPipeline* m_render_pass_pso = nullptr;

		u32 m_texture_upload_buffer_offset = 0;
		u32 m_texture_upload_buffer_cycle_count = 0;
		u32 m_buffer_upload_buffer_offset = 0;
		u32 m_buffer_upload_buffer_cycle_count = 0;
	};
}
