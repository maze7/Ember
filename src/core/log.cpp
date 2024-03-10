#include "core/log.h"

// construct the static mutex
std::mutex Ember::Log::m_mutex = std::mutex();