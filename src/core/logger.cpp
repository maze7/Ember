#include "core/logger.h"

// construct the static mutex
std::mutex Ember::Log::s_mutex = std::mutex();