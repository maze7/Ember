#pragma once

#include <ember/core/log.h>

#ifdef EMBER_HAS_PLATFORM
	#include <ember/platform/platform.h>
#endif

#ifdef EMBER_HAS_GRAPHICS
	#include <ember/graphics/renderer.h>
#endif

#ifdef EMBER_HAS_AUDIO
	#include <ember/audio/device.h>
#endif
