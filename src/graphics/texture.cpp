#include "graphics/texture.h"

using namespace Ember;

Texture::Texture(u32 width, u32 height, TextureFormat format)
	: m_size(width, height), m_format(format) {
	m_resource = render_device->create_texture(width, height, format, nullptr);
}

Texture::~Texture() {
	render_device->destroy_texture(m_resource);
}