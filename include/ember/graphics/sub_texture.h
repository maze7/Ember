#pragma once
#include <vec2.hpp>

#include "core/common.h"
#include "math/rect.h"

namespace Ember
{
	class Texture;

	struct SubTexture
	{
		// Reference to the Texture
		Ref<Texture> texture;

		// Source rectangle, in pixels
		Rectf source;

		// Frame rectangle, in pixels. This describes padding around the image.
		// This is useful for drawing images that have been trimmed. Ex. if the source
		// is 32,32, but the original image was 64,64, the frame could be -16, -16, 64, 64
		Rectf frame;

		// `uv` coordinates are automatically assigned through `update` method
		glm::vec2 uv[4];

		// `tex_coords` are automatically assigned through `update` method
		glm::vec2 tex_coords[4];

		SubTexture();

		explicit SubTexture(const Ref<Texture>& tex);

		SubTexture(const Ref<Texture>& tex, Rectf source);

		SubTexture(const Ref<Texture>& tex, Rectf source, Rectf frame);

		// updates the  `draw_coords` and `tex_coords`
		void update();

		// returns the width of the image
		[[nodiscard]] float width() const { return frame.w; }

		// returns the height of the image
		[[nodiscard]] float height() const { return frame.h; }

		// returns a new SubTexture cropped from this SubTexture (referencing the same Texture)
		[[nodiscard]] SubTexture crop(const Rectf& clip) const;
	};
}
