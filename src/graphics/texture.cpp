#include "graphics/texture.h"

using namespace Ember;

Texture::Texture(u32 width, u32 height, TextureFormat format, Target* target)
	: m_size(width, height), m_format(format), m_is_target_attachment(target != nullptr){
	m_resource = render_device->create_texture(width, height, format, target);
}

Texture::~Texture() {
	if (!m_is_target_attachment)
		render_device->destroy_texture(m_resource);
}