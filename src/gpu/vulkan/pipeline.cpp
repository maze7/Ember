// Pipelines share the heap's layout, but their pool indices never reach shaders. Creation asks
// the driver to compile on the calling thread, so keep it away from work a frame must wait for.
// Shader modules are temporary inputs and can be released once pipeline creation returns.
// Destruction invalidates the handle now and defers the Vulkan object's release; replacement
// builds a new pipeline and swaps it behind the handle, which is how a shader hot reloads.

#include <ember/gpu/gpu.h>

#include <gpu/vulkan/backend.h>

namespace ember::gpu
{
	namespace
	{
		VkPrimitiveTopology to_vk(PrimitiveTopology topology)
		{
			switch (topology)
			{
				case PrimitiveTopology::TriangleList:
					return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
				case PrimitiveTopology::TriangleStrip:
					return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
				case PrimitiveTopology::LineList:
					return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
				case PrimitiveTopology::PointList:
					return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
				case PrimitiveTopology::Count:
					break;
			}

			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}

		VkCullModeFlags to_vk(CullMode cull)
		{
			switch (cull)
			{
				case CullMode::None:
					return VK_CULL_MODE_NONE;
				case CullMode::Back:
					return VK_CULL_MODE_BACK_BIT;
				case CullMode::Front:
					return VK_CULL_MODE_FRONT_BIT;
				case CullMode::Count:
					break;
			}

			return VK_CULL_MODE_NONE;
		}

		// All presets write every channel. Blended presets add weighted source and destination
		// values; opaque rendering disables blending and writes the source directly.
		VkPipelineColorBlendAttachmentState to_vk(BlendPreset preset)
		{
			VkPipelineColorBlendAttachmentState state{
			    .blendEnable = VK_TRUE,
			    .colorBlendOp = VK_BLEND_OP_ADD,
			    .alphaBlendOp = VK_BLEND_OP_ADD,
			    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
				                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
			};

			switch (preset)
			{
				case BlendPreset::Opaque:
				case BlendPreset::Count:
					state.blendEnable = VK_FALSE;
					break;

				case BlendPreset::AlphaBlend:
					state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					break;

				case BlendPreset::Additive:
					state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
					state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
					state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
					state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					break;

				case BlendPreset::PremultipliedAlpha:
					state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
					state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
					state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
					break;
			}

			return state;
		}

		// Wrap the shader code for pipeline creation. Definition validation has already checked
		// that the byte count and address meet SPIR-V's four-byte word alignment.
		VkShaderModule create_module(const vk::Context& ctx, const ShaderStageDef& stage,
		    const char* pipeline_name)
		{
			const VkShaderModuleCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			    .codeSize = stage.code.size(),
			    .pCode = (const u32*)stage.code.data(),
			};

			VkShaderModule module = VK_NULL_HANDLE;
			if (const VkResult result =
			        vkCreateShaderModule(ctx.device, &info, &ctx.callbacks, &module);
			    result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: pipeline '{}' shader module creation failed: {}", pipeline_name,
				    vk::result_name(result));
				return VK_NULL_HANDLE;
			}

			return module;
		}

		VkPipelineShaderStageCreateInfo stage_info(VkShaderStageFlagBits stage,
		    VkShaderModule module, const char* entry)
		{
			return {
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			    .stage = stage,
			    .module = module,
			    .pName = entry,
			};
		}

		// Adopt the compiled pipeline under the pool lock. If the pool is full, release it here
		// because no caller has received a handle that could submit it.
		template <typename P>
		typename P::Handle adopt(vk::Backend& b, P& pool, VkPipeline pipeline, const char* name)
		{
			b.resources_lock.lock();
			const typename P::Handle handle = pool.emplace(pipeline);
			b.resources_lock.unlock();

			if (handle.is_null())
			{
				EMBER_ERROR("gpu: pipeline pool full, '{}' refused", name);
				vkDestroyPipeline(b.ctx.device, pipeline, &b.ctx.callbacks);
				return {};
			}

			vk::set_name(b.ctx, VK_OBJECT_TYPE_PIPELINE, (u64)pipeline, name);
			return handle;
		}

		// Put a freshly built pipeline behind an existing handle and retire the one it replaces.
		// A list recording meanwhile binds whichever it reads; the graveyard keeps the old one
		// until its frames complete.
		template <typename P>
		bool swap(vk::Backend& b, P& pool, typename P::Handle handle, VkPipeline pipeline,
		    const char* name)
		{
			b.resources_lock.lock();
			vk::PipelineData* data = pool.get(handle);
			if (data != nullptr)
			{
				const VkPipeline old = data->pipeline.exchange(pipeline, std::memory_order_acq_rel);
				vk::bury(b, {.kind = vk::Dead::Kind::Pipeline, .handle = (u64)old});
			}
			b.resources_lock.unlock();

			if (data == nullptr)
			{
				EMBER_ERROR("gpu: replace of a stale pipeline with '{}'", name);
				vkDestroyPipeline(b.ctx.device, pipeline, &b.ctx.callbacks);
				return false;
			}

			vk::set_name(b.ctx, VK_OBJECT_TYPE_PIPELINE, (u64)pipeline, name);
			return true;
		}

		// Recorded commands hold the Vulkan pipeline directly, so its CPU pool slot can be
		// reused as soon as the handle is invalidated. Keep the Vulkan object in the graveyard
		// until the frames that could use it have completed.
		template <typename P>
		void drop(vk::Backend& b, P& pool, typename P::Handle handle)
		{
			b.resources_lock.lock();

			if (const vk::PipelineData* data = pool.get(handle))
			{
				vk::bury(b, {.kind = vk::Dead::Kind::Pipeline,
				                .handle = (u64)data->pipeline.load(std::memory_order_relaxed)});
				(void)pool.erase(handle);
			}

			b.resources_lock.unlock();
		}

		// Compile a graphics pipeline for def, or return null with the failure logged.
		VkPipeline build(const vk::Backend& b, const GraphicsPipelineDef& def)
		{
			const vk::Context& ctx = b.ctx;
			if (def.fill == FillMode::Wireframe && !ctx.caps.wireframe)
			{
				EMBER_ERROR("gpu: pipeline '{}' wants wireframe, which the adapter lacks",
				    def.name);
				return VK_NULL_HANDLE;
			}

			const VkShaderModule vertex = create_module(ctx, def.vertex, def.name);
			const VkShaderModule fragment = create_module(ctx, def.fragment, def.name);
			if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE)
			{
				vkDestroyShaderModule(ctx.device, vertex, &ctx.callbacks);
				vkDestroyShaderModule(ctx.device, fragment, &ctx.callbacks);
				return VK_NULL_HANDLE;
			}

			const VkPipelineShaderStageCreateInfo stages[] = {
			    stage_info(VK_SHADER_STAGE_VERTEX_BIT, vertex, def.vertex.entry),
			    stage_info(VK_SHADER_STAGE_FRAGMENT_BIT, fragment, def.fragment.entry),
			};

			// Shaders fetch vertices from storage buffers, so there are no vertex bindings to
			// describe.
			const VkPipelineVertexInputStateCreateInfo vertex_input{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			};

			const VkPipelineInputAssemblyStateCreateInfo input_assembly{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			    .topology = to_vk(def.topology),
			};

			// Reserve one viewport and scissor here. Command recording supplies their values for
			// each render pass and may change them later.
			const VkPipelineViewportStateCreateInfo viewport{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			    .viewportCount = 1,
			    .scissorCount = 1,
			};

			const bool biased = def.depth_bias.constant != 0.0f || def.depth_bias.slope != 0.0f;

			// Pass through the engine's winding choice. It shares its screen-space convention with
			// the negative-height viewport used during command recording.
			const VkPipelineRasterizationStateCreateInfo raster{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			    .depthClampEnable = def.depth_clamp ? VK_TRUE : VK_FALSE,
			    .polygonMode =
			        def.fill == FillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
			    .cullMode = to_vk(def.cull),
			    .frontFace = def.front == FrontFace::CounterClockwise
			                     ? VK_FRONT_FACE_COUNTER_CLOCKWISE
			                     : VK_FRONT_FACE_CLOCKWISE,
			    .depthBiasEnable = biased ? VK_TRUE : VK_FALSE,
			    .depthBiasConstantFactor = def.depth_bias.constant,
			    .depthBiasClamp = def.depth_bias.clamp,
			    .depthBiasSlopeFactor = def.depth_bias.slope,
			    .lineWidth = 1.0f,
			};

			const VkPipelineMultisampleStateCreateInfo multisample{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			};

			const VkPipelineDepthStencilStateCreateInfo depth{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			    .depthTestEnable = def.depth_test ? VK_TRUE : VK_FALSE,
			    .depthWriteEnable = def.depth_write ? VK_TRUE : VK_FALSE,
			    .depthCompareOp = vk::to_vk(def.depth_compare),
			};

			VkPipelineColorBlendAttachmentState blends[MAX_COLOR_ATTACHMENTS];
			VkFormat color_formats[MAX_COLOR_ATTACHMENTS];
			for (u32 i = 0; i < def.color_count; ++i)
			{
				blends[i] = to_vk(def.blend);
				color_formats[i] = vk::to_vk(def.color_formats[i]);
			}

			const VkPipelineColorBlendStateCreateInfo blend{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			    .attachmentCount = def.color_count,
			    .pAttachments = blends,
			};

			const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
			    VK_DYNAMIC_STATE_SCISSOR};
			const VkPipelineDynamicStateCreateInfo dynamic{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			    .dynamicStateCount = 2,
			    .pDynamicStates = dynamic_states,
			};

			// Dynamic rendering needs attachment formats here instead of a VkRenderPass object.
			const VkPipelineRenderingCreateInfo rendering{
			    .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			    .colorAttachmentCount = def.color_count,
			    .pColorAttachmentFormats = color_formats,
			    .depthAttachmentFormat = vk::to_vk(def.depth_format),
			};

			const VkGraphicsPipelineCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			    .pNext = &rendering,
			    .stageCount = 2,
			    .pStages = stages,
			    .pVertexInputState = &vertex_input,
			    .pInputAssemblyState = &input_assembly,
			    .pViewportState = &viewport,
			    .pRasterizationState = &raster,
			    .pMultisampleState = &multisample,
			    .pDepthStencilState = &depth,
			    .pColorBlendState = &blend,
			    .pDynamicState = &dynamic,
			    .layout = b.heap.pipeline_layout,
			};

			VkPipeline pipeline = VK_NULL_HANDLE;
			const VkResult result = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &info,
			    &ctx.callbacks, &pipeline);

			vkDestroyShaderModule(ctx.device, vertex, &ctx.callbacks);
			vkDestroyShaderModule(ctx.device, fragment, &ctx.callbacks);

			if (result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: pipeline '{}' creation failed: {}", def.name,
				    vk::result_name(result));
				return VK_NULL_HANDLE;
			}

			return pipeline;
		}

		// Compile a compute pipeline for def, or return null with the failure logged.
		VkPipeline build(const vk::Backend& b, const ComputePipelineDef& def)
		{
			const vk::Context& ctx = b.ctx;
			const VkShaderModule shader = create_module(ctx, def.shader, def.name);
			if (shader == VK_NULL_HANDLE)
			{
				return VK_NULL_HANDLE;
			}

			const VkComputePipelineCreateInfo info{
			    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			    .stage = stage_info(VK_SHADER_STAGE_COMPUTE_BIT, shader, def.shader.entry),
			    .layout = b.heap.pipeline_layout,
			};

			VkPipeline pipeline = VK_NULL_HANDLE;
			const VkResult result = vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &info,
			    &ctx.callbacks, &pipeline);

			vkDestroyShaderModule(ctx.device, shader, &ctx.callbacks);

			if (result != VK_SUCCESS)
			{
				EMBER_ERROR("gpu: compute pipeline '{}' creation failed: {}", def.name,
				    vk::result_name(result));
				return VK_NULL_HANDLE;
			}

			return pipeline;
		}
	}

	GraphicsPipelineHandle create_graphics_pipeline(const GraphicsPipelineDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: pipeline '{}' has an invalid def", def.name);
			return {};
		}

		const VkPipeline pipeline = build(*b, def);
		if (pipeline == VK_NULL_HANDLE)
		{
			return {};
		}

		return adopt(*b, b->graphics_pipelines, pipeline, def.name);
	}

	ComputePipelineHandle create_compute_pipeline(const ComputePipelineDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return {};
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: compute pipeline '{}' has an invalid def", def.name);
			return {};
		}

		const VkPipeline pipeline = build(*b, def);
		if (pipeline == VK_NULL_HANDLE)
		{
			return {};
		}

		return adopt(*b, b->compute_pipelines, pipeline, def.name);
	}

	bool replace(GraphicsPipelineHandle handle, const GraphicsPipelineDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return false;
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: pipeline '{}' has an invalid def", def.name);
			return false;
		}

		const VkPipeline pipeline = build(*b, def);
		return pipeline != VK_NULL_HANDLE &&
		       swap(*b, b->graphics_pipelines, handle, pipeline, def.name);
	}

	bool replace(ComputePipelineHandle handle, const ComputePipelineDef& def)
	{
		vk::Backend* b = vk::backend();
		if (b == nullptr)
		{
			return false;
		}

		if (!is_valid(def))
		{
			EMBER_ERROR("gpu: compute pipeline '{}' has an invalid def", def.name);
			return false;
		}

		const VkPipeline pipeline = build(*b, def);
		return pipeline != VK_NULL_HANDLE &&
		       swap(*b, b->compute_pipelines, handle, pipeline, def.name);
	}

	void destroy(GraphicsPipelineHandle handle)
	{
		if (vk::Backend* b = vk::backend())
		{
			drop(*b, b->graphics_pipelines, handle);
		}
	}

	void destroy(ComputePipelineHandle handle)
	{
		if (vk::Backend* b = vk::backend())
		{
			drop(*b, b->compute_pipelines, handle);
		}
	}

	bool is_valid(GraphicsPipelineHandle handle)
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->graphics_pipelines.get(handle) != nullptr;
	}

	bool is_valid(ComputePipelineHandle handle)
	{
		const vk::Backend* b = vk::backend();
		return b != nullptr && b->compute_pipelines.get(handle) != nullptr;
	}
}
