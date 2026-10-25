#pragma once

#include <ember/core/frame.h>
#include <ember/core/jobs.h>
#include <ember/core/log.h>
#include <ember/memory/memory.h>

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

namespace ember
{
	struct EngineDef
	{
		memory::MemoryDef memory;
		LogDef log;
		jobs::JobsDef jobs;
		FramesDef frames;

#ifdef EMBER_HAS_PLATFORM
		bool with_platform = true;
		platform::PlatformDef platform;
#endif

#ifdef EMBER_HAS_GPU
		bool with_gpu = true;
		gpu::GpuDef gpu;
#endif
	};

	// Initializes engine subsystems in correct order. If a system fails, the reason is logged and
	// shutdown is run in reverse-initialization order.
	bool initialize(const EngineDef& def = {});

	// Stops whatever initialize started, in reverse. Every job must have completed.
	void shutdown();
}
