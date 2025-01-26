#include "graphics/texture.h"
#include "stb_image.h"

using namespace Ember;

Texture::Texture(u32 width, u32 height, TextureFormat format, Target* target)
	: m_size(width, height), m_format(format), m_is_target_attachment(target != nullptr){
	m_handle = render_device->create_texture(width, height, format, target);
}

Texture::Texture(u32 width, u32 height, std::span<std::byte> pixels)
	: m_size(width, height), m_format(TextureFormat::R8G8B8A8), m_is_target_attachment(false) {
	m_handle = render_device->create_texture(width, height, m_format, nullptr);
	set_data<std::byte>(pixels);
}

Texture::~Texture() {
	if (!m_is_target_attachment)
		render_device->dispose_texture(m_handle);
}

Ref<Texture> Texture::load(std::string_view path) {
	int width, height, channels;

	// stbi_set_flip_vertically_on_load(true);

	// load the image data using stb_image
	stbi_uc* data = stbi_load(path.data(), &width, &height, &channels, 4);
	if (!data) {
		throw Exception("Failed to load texture: " + std::string(path));
	}

	// create the Texture
	auto texture = make_ref<Texture>((u32) width, (u32) height, std::span((std::byte*) data, width * height * 4));

	// free the stb_image buffer
	stbi_image_free(data);

	// explicitly move the texture to avoid destruction
	return texture;
}
