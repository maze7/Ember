#pragma once

#include <fmt/format.h>
#include <iostream>
#include <mutex>

namespace ember
{
	class Log
	{
	public:
		// Log a trace-level message
		template <typename ...Args>
		static void trace(fmt::format_string<Args...> fmt, Args&&... args)
		{
			log("trace", "36", std::forward(fmt), std::forward<Args>(args)...) << std::endl;
		}

		// Log an info-level message
		template <typename ...Args>
		static void info(fmt::format_string<Args...> fmt, Args&&... args)
		{
			log("info", "32", std::forward(fmt), std::forward<Args>(args)...) << std::endl;
		}

		// Log a warning-level message
		template <typename ...Args>
		static void warn(fmt::format_string<Args...> fmt, Args&&... args)
		{
			log("warn", "33", std::forward(fmt), std::forward<Args>(args)...) << std::endl;
		}

		// Log an error-level message
		template <typename ...Args>
		static void error(fmt::format_string<Args...> fmt, Args&&... args)
		{
			log("error", "31", std::forward(fmt), std::forward<Args>(args)...) << std::endl;
		}

	private:
		// ASCII Encodes a string to be a certain color, utility function used by above log functions
		static constexpr const char* colored(const char* str, const char* color) {
			return (std::string("\033[") + color + "m" + str + "\033[m").c_str();
		}

		// Generic log builder, handles label, color and formatting of a single log line.
		template <typename ...Args>
		static void log(const char* label, const char* color, fmt::format_string<Args...> fmt, Args&&... args)
		{
			// lock or block until mutex is available
			std::scoped_lock lock(s_mutex);
			std::cout << "[" << colored(label, color) << "] " << fmt::format(fmt, std::forward<Args>(args)...) << std::endl;
		}

		// static mutex is used to ensure multiple loggers or threads do not write at the same time
		static std::mutex s_mutex;
	};
}