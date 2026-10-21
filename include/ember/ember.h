#pragma once

#include <ember/core/log.h>

#ifdef EMBER_HAS_PLATFORM
	#include <ember/platform/platform.h>
#endif

#ifdef EMBER_HAS_GPU
	#include <ember/gpu/commands.h>
	#include <ember/gpu/gpu.h>
#endif

#ifdef EMBER_HAS_AUDIO
	#include <ember/audio/device.h>
#endif
