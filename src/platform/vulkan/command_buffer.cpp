#include "core/common.h"
#include "vulkan/vulkan.hpp"
#include "platform/vulkan/command_buffer.h"
#include "platform/vulkan/render_device.h"

using namespace Ember;

void CommandBuffer::begin() {
	// Ensure we haven't already begun recording to this command buffer
	EMBER_ASSERT(!m_recording);

	m_cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	m_recording = true;

	// TODO: For now we begin the render pass here. Render passes should probably be abstracted and moved to some form of
	// begin_pass and end_pass functions within the command buffer. This way we can submit multiple passes at once.
	vk::RenderPassBeginInfo begin_info(m_pass, m_framebuffer, vk::Rect2D({ 0, 0 }, m_gpu->swapchain_extent()), m_clears);
	m_cmd.beginRenderPass(begin_info, vk::SubpassContents::eInline);
}

void CommandBuffer::end() {
	EMBER_ASSERT(m_recording);

	m_cmd.endRenderPass();
	m_cmd.end();
	m_recording = false;
}

void CommandBuffer::reset() {
	m_recording = false;
	m_cmd.reset();
}

void CommandBuffer::set_viewport(i32 x, i32 y, i32 width, i32 height) {
	EMBER_ASSERT(m_recording);
	m_cmd.setViewport(0, vk::Viewport(x, y, width, height, 0, 1));
}

void CommandBuffer::set_scissor(i32 x, i32 y, i32 width, i32 height) {
	EMBER_ASSERT(m_recording);
	m_cmd.setScissor(0, vk::Rect2D({ x, y}, { (u32) width, (u32) height }));
}

void CommandBuffer::clear(f32 red, f32 green, f32 blue, f32 alpha) {
	m_clears[0].color = { red, green, blue, alpha };
}
