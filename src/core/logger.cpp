#include "core/logger.h"

// construct the static mutex
std::mutex ember::Logger::s_mutex = std::mutex();