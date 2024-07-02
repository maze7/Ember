#include "core/logger.h"

// construct the static mutex
std::mutex Ember::Logger::s_mutex = std::mutex();