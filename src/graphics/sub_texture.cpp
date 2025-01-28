#include "graphics/sub_texture.h"
#include "graphics/texture.h"

using namespace Ember;

SubTexture::SubTexture() {}

SubTexture::SubTexture(const Ref<Texture>& texture)
	: SubTexture(texture, Rectf(0, 0, (float)texture->size().x, (float)texture->size().y)) {}

SubTexture::SubTexture(const Ref<Texture>& tex, Rectf src)
	: SubTexture(tex, src, Rectf(0, 0, src.w, src.h)) {}

SubTexture::SubTexture(const Ref<Texture>& tex, Rectf src, Rectf frame)
	: texture(tex), source(src), frame(frame) {
	update();
}

void SubTexture::update() {
	tex_coords[0].x = -frame.x;
	tex_coords[0].y = -frame.y;
	tex_coords[1].x = -frame.x + source.w;
	tex_coords[1].y = -frame.y;
	tex_coords[2].x = -frame.x + source.w;
	tex_coords[2].y = -frame.y + source.h;
	tex_coords[3].x = -frame.x;
	tex_coords[3].y = -frame.y + source.h;

	if (texture) {
		float uvx = 1.0f / texture->size().x;
		float uvy = 1.0f / texture->size().y;

		uv[0].x = source.x * uvx;
		uv[0].y = source.y * uvy;
		uv[1].x = (source.x + source.w) * uvx;
		uv[1].y = source.y * uvy;
		uv[2].x = (source.x + source.w) * uvx;
		uv[2].y = (source.y + source.h) * uvy;
		uv[3].x = source.x * uvx;
		uv[3].y = (source.y + source.h) * uvy;
	}
}

SubTexture SubTexture::crop(const Rectf& clip) const
{
    // Rectf dest_source = (clip + source.top_left() + frame.top_left()).overlap_rect(source);
    //
    // Rectf dest_frame;
    // dest_frame.x = Calc::min(0.0f, frame.x + clip.x);
    // dest_frame.y = Calc::min(0.0f, frame.y + clip.y);
    // dest_frame.w = clip.w;
    // dest_frame.h = clip.h;
    //
    // return SubTexture(texture, dest_source, dest_frame);
}
