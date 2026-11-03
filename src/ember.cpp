#include <ember/ember.h>

namespace ember
{
	bool initialize(const EngineDef& def)
	{
		// Memory must be initialized first as all other subsystems rely on allocating from it
		if (!memory::initialize(def.memory))
			return false;

		// Logging second, so that subsequent subsystems can use it
		if (!log::initialize(def.log) || !jobs::initialize(def.jobs))
		{
			shutdown();
			return false;
		}

		frames::initialize(def.frames);

#ifdef EMBER_HAS_PLATFORM
		if (def.with_platform && !platform::initialize(def.platform))
		{
			shutdown();
			return false;
		}
#endif

#ifdef EMBER_HAS_GPU
		if (def.with_gpu && !gpu::initialize(def.gpu))
		{
			shutdown();
			return false;
		}
#endif

		return true;
	}

	void shutdown()
	{
#ifdef EMBER_HAS_GPU
		gpu::shutdown();
#endif
#ifdef EMBER_HAS_PLATFORM
		platform::shutdown();
#endif
		frames::shutdown();
		jobs::shutdown();
		log::shutdown();
		memory::shutdown();
	}
}
