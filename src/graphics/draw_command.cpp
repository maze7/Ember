#include "graphics/draw_command.h"

using namespace Ember;

void DrawCommand::submit() {
	render_device->draw(*this);
}
