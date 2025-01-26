#include "graphics/target.h"

using namespace Ember;

Target::Target(u32 width, u32 height)
	: Target(width, height, { TextureFormat::Color }) {}

Target::Target(u32 width, u32 height, std::initializer_list<TextureFormat> attachments) {
	EMBER_ASSERT(attachments.size() > 0);

	m_resource = render_device->create_target(width, height);
	m_rect = Quad<u32>(0, 0, width, height);
	for (auto format : attachments)
		m_attachments.emplace_back(width, height, format, this);
}

Target::~Target() {
	// destroy attachments connected to this target
	for (auto& attachment : m_attachments)
		render_device->dispose_texture(attachment.handle());

	// destroy the target itself
	render_device->dispose_target(m_resource);
}
